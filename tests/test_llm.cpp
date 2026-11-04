/*
 * llm 接入层测试（mock 模式，不打真实 API）：
 * DeepSeekClient 注入 fake transport 验证缓存键/重试/响应解析；
 * LlmExtractor 宽容解析与规则回退；LlmAnswerer 弃答判定；LlmJudge 三种口径；
 * fake client 跑通 ingest→answer→judge 夹具级端到端链路。
 * 集成测试（真实 API）默认跳过，设 AME_RUN_LLM_IT=1 才执行。
 */
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "ame/bench/bench.h"
#include "ame/core/json.h"
#include "ame/engine/engine.h"
#include "ame/llm/components.h"
#include "ame/llm/llm.h"
#include "../llm/src/retry_backoff.h"
#include "test_util.hpp"
#include "test_temp_dir.hpp"

using namespace ame;

using Milliseconds = std::chrono::milliseconds;

class VirtualRetryClock {
 public:
  explicit VirtualRetryClock(Milliseconds delay = Milliseconds{10}) : delay_(delay) {}

  LlmRetryRuntime runtime() {
    return {[this] { return now(); },
            [this](Milliseconds duration) { sleep(duration); },
            [this](int attempt) { return delay(attempt); }};
  }

  void advance(Milliseconds duration) {
    const std::lock_guard<std::mutex> lock(mutex_);
    now_ += duration;
  }

  std::vector<Milliseconds> waits;
  std::vector<int> delay_attempts;

 private:
  LlmRetryRuntime::Clock::time_point now() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return now_;
  }

  void sleep(Milliseconds duration) {
    const std::lock_guard<std::mutex> lock(mutex_);
    waits.push_back(duration);
    now_ += duration;
  }

  Milliseconds delay(int attempt) {
    const std::lock_guard<std::mutex> lock(mutex_);
    delay_attempts.push_back(attempt);
    return delay_;
  }

  std::mutex mutex_;
  LlmRetryRuntime::Clock::time_point now_{};
  Milliseconds delay_;
};

static std::string llm_response(const std::string& content, const std::string& finish = "length") {
  return "{\"choices\":[{\"finish_reason\":" + Json::string(finish).dump() +
         ",\"message\":{\"content\":" + Json::string(content).dump() + "}}]}";
}

static int request_token_budget(const std::string& body) {
  return static_cast<int>(Json::parse(body)["max_tokens"].as_num());
}

static LlmTransport length_response(std::vector<int>& budgets, const std::string& content = "partial") {
  return [&budgets, content](const std::string& body, std::string* out) {
    budgets.push_back(request_token_budget(body));
    *out = llm_response(content);
    return 200;
  };
}

static void check_retry_window(int attempt, int ceiling_ms) {
  std::mt19937 generator{42};
  const auto first = detail::retry_backoff(attempt, generator).count();
  bool varied = false;
  bool upper_half = false;
  for (int sample = 0; sample < 64; ++sample) {
    const auto delay = detail::retry_backoff(attempt, generator).count();
    CHECK(delay >= 0 && delay <= ceiling_ms);
    varied = varied || delay != first;
    upper_half = upper_half || delay > ceiling_ms / 2;
  }
  CHECK(varied);
  CHECK(upper_half);
}

static void test_retry_backoff() {
  std::mt19937 generator{42};
  CHECK(detail::retry_backoff(0, generator).count() == 0);
  CHECK(detail::retry_backoff(-1, generator).count() == 0);
  check_retry_window(1, 1000);
  check_retry_window(2, 2000);
  check_retry_window(3, 4000);
  check_retry_window(4, 8000);
  check_retry_window(std::numeric_limits<int>::max(), 8000);
}

static void test_retry_entropy_failure() {
  int calls = 0;
  const auto seed = detail::retry_seed([&calls]() -> std::mt19937::result_type {
    ++calls;
    throw std::runtime_error("Entropy source unavailable");
  });
  std::mt19937 generator{seed};
  const auto delay = detail::retry_backoff(1, generator).count();
  CHECK(calls == 1);
  CHECK(delay >= 0 && delay <= 1000);
}

static void test_retry_exhaustion() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  const int failures[] = {0, 429, 500, 503, 0};
  int calls = 0;
  client.set_transport([&calls, &failures](const std::string&, std::string*) {
    const int index = calls++;
    return index < 5 ? failures[index] : 400;
  });
  CHECK(client.chat({{"user", "retry failure"}}, {}).empty());
  CHECK(calls == 5);
  CHECK(client.api_calls() == 5);
  CHECK(clock.delay_attempts == std::vector<int>({1, 2, 3, 4}));
  CHECK(clock.waits == std::vector<Milliseconds>(4, Milliseconds{10}));
}

static void test_chat_batch_retries() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::mutex mutex;
  std::map<std::string, int> attempts;
  client.set_transport([&mutex, &attempts](const std::string& body, std::string* out) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (++attempts[body] == 1) return 503;
    *out = R"({"choices":[{"message":{"content":"retried"}}]})";
    return 200;
  });
  std::vector<ChatRequest> requests;
  for (int i = 0; i < 8; ++i) requests.push_back({{{"user", "retry-" + std::to_string(i)}}, {}});
  const auto results = client.chat_batch(requests, 8);
  CHECK(results.size() == requests.size());
  for (const auto& result : results) CHECK(result == "retried");
  CHECK(attempts.size() == requests.size());
  for (const auto& entry : attempts) CHECK(entry.second == 2);
  CHECK(client.api_calls() == 16);
  CHECK(client.cache_hits() == 0);
  CHECK(clock.delay_attempts == std::vector<int>(8, 1));
  CHECK(clock.waits == std::vector<Milliseconds>(8, Milliseconds{10}));
}

static void test_client_cache_and_retry() {
  const ame_test::TempDirectory cache;
  const std::string dir = cache.root().string();

  DeepSeekClient c("https://example.invalid", "test-key-not-real", "m", dir);
  int calls = 0;
  c.set_transport([&calls](const std::string& body, std::string* out) -> int {
    ++calls;
    CHECK(body.find("\"model\":\"m\"") != std::string::npos);
    *out = R"({"choices":[{"message":{"content":"hello"}}]})";
    return 200;
  });
  // 首次调用走 transport，第二次同请求命中缓存
  CHECK(c.chat({{"user", "hi"}}, {}) == "hello");
  CHECK(c.chat({{"user", "hi"}}, {}) == "hello");
  CHECK(calls == 1);
  CHECK(c.api_calls() == 1 && c.cache_hits() == 1);
  // 缓存文件存在且 content 不含 key
  int files = 0;
  for (auto it : std::filesystem::directory_iterator(dir)) {
    (void)it;
    ++files;
  }
  CHECK(files == 1);

  // 重试：两次 429 后 200
  VirtualRetryClock clock;
  DeepSeekClient c2("https://example.invalid", "k", "m", "");
  c2.set_retry_runtime(clock.runtime());
  int tries = 0;
  c2.set_transport([&tries](const std::string&, std::string* out) -> int {
    if (++tries < 3) return 429;
    *out = R"({"choices":[{"message":{"content":"ok"}}]})";
    return 200;
  });
  CHECK(c2.chat({{"user", "x"}}, {}) == "ok");
  CHECK(tries == 3);
  CHECK(clock.delay_attempts == std::vector<int>({1, 2}));
  CHECK(clock.waits == std::vector<Milliseconds>(2, Milliseconds{10}));

  // 不可重试错误（400）立即失败
  DeepSeekClient c3("https://example.invalid", "k", "m", "");
  int tries3 = 0;
  c3.set_transport([&tries3](const std::string&, std::string*) -> int {
    ++tries3;
    return 400;
  });
  CHECK(c3.chat({{"user", "x"}}, {}).empty());
  CHECK(tries3 == 1);
}

static void test_chat_batch() {
  DeepSeekClient c("https://example.invalid", "k", "m", "");
  std::atomic<int> calls{0};
  c.set_transport([&calls](const std::string& body, std::string* out) -> int {
    ++calls;
    // 按问题内容回显，验证结果与请求同序（取 q- 后的数字编号）
    size_t p = body.find("q-");
    size_t e = body.find('"', p);
    *out = "{\"choices\":[{\"message\":{\"content\":\"ans-" + body.substr(p + 2, e - p - 2) +
           "\"}}]}";
    return 200;
  });
  std::vector<ChatRequest> reqs;
  for (int i = 0; i < 16; ++i)
    reqs.push_back({{{"user", "q-" + std::to_string(i)}}, {}});
  auto resps = c.chat_batch(reqs, 8);
  CHECK(calls == 16);
  CHECK(c.api_calls() == 16);
  CHECK(resps.size() == 16);
  for (int i = 0; i < 16; ++i)
    CHECK(resps[i] == "ans-" + std::to_string(i));
}

static void test_token_expansion_cap() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::vector<int> budgets;
  client.set_transport(length_response(budgets));
  ChatOptions options;
  options.max_tokens = 20000;
  const auto result = client.chat_result({{"user", "capped expansion"}}, options);
  CHECK(budgets == std::vector<int>({20000, 32768}));
  CHECK(result.status == ChatStatus::Truncated);
  CHECK(result.content == "partial");
  CHECK(client.api_calls() == 2);
  CHECK(clock.waits.empty());
  CHECK(clock.delay_attempts.empty());
}

static void test_default_token_expansion() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::vector<int> budgets;
  client.set_transport(length_response(budgets));
  const auto result = client.chat_result({{"user", "default expansion"}}, {});
  CHECK(budgets == std::vector<int>({1024, 2048, 4096, 8192, 16384, 32768}));
  CHECK(result.status == ChatStatus::Truncated);
  CHECK(result.content == "partial");
  CHECK(client.api_calls() == 6);
  CHECK(clock.waits.empty());
}

static void test_expansion_with_transient_failures() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::vector<int> budgets;
  client.set_transport([&budgets](const std::string& body, std::string* out) {
    budgets.push_back(request_token_budget(body));
    if (budgets.size() == 1) return 429;
    if (budgets.size() == 3) return 503;
    *out = llm_response("partial");
    return 200;
  });
  const auto result = client.chat_result({{"user", "mixed retry budgets"}}, {});
  CHECK(budgets == std::vector<int>({1024, 1024, 2048, 2048, 4096, 8192, 16384, 32768}));
  CHECK(result.status == ChatStatus::Truncated);
  CHECK(result.content == "partial");
  CHECK(client.api_calls() == 8);
  CHECK(clock.delay_attempts == std::vector<int>({1, 2}));
  CHECK(clock.waits == std::vector<Milliseconds>(2, Milliseconds{10}));
}

static void test_maximum_expansion_budget() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::vector<int> budgets;
  client.set_transport([&budgets](const std::string& body, std::string* out) {
    budgets.push_back(request_token_budget(body));
    if (budgets.size() <= 4) return 503;
    *out = llm_response("partial");
    return 200;
  });
  ChatOptions options;
  options.max_tokens = 1;
  const auto result = client.chat_result({{"user", "maximum expansion"}}, options);
  CHECK(budgets.size() == 20);
  CHECK(std::count(budgets.begin(), budgets.end(), 1) == 5);
  CHECK(!budgets.empty() && budgets.back() == 32768);
  CHECK(result.status == ChatStatus::Truncated);
  CHECK(result.content == "partial");
  CHECK(client.api_calls() == 20);
  CHECK(clock.delay_attempts == std::vector<int>({1, 2, 3, 4}));
}

static void test_transient_budget_preserves_partial() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string* out) {
    if (++calls % 2 != 0) return 503;
    *out = llm_response("part-" + std::to_string(calls / 2));
    return 200;
  });
  const auto result = client.chat_result({{"user", "cumulative failures"}}, {});
  CHECK(result.status == ChatStatus::Failed);
  CHECK(result.content == "part-4");
  CHECK(calls == 9);
  CHECK(client.api_calls() == 9);
  CHECK(clock.delay_attempts == std::vector<int>({1, 2, 3, 4}));
}

static void test_partial_cache_and_json_policy() {
  const ame_test::TempDirectory cache;
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  client.set_retry_runtime(clock.runtime());
  std::vector<int> budgets;
  const std::string partial = "{\"incomplete\":";
  client.set_transport(length_response(budgets, partial));
  ChatOptions options;
  options.max_tokens = 32768;
  const std::vector<ChatMessage> messages{{"user", "partial policy"}};
  const auto text = client.chat_result(messages, options);
  CHECK(text.status == ChatStatus::Truncated && text.content == partial);
  CHECK(client.chat(messages, options) == partial);
  options.json = true;
  const auto json = client.chat_result(messages, options);
  CHECK(json.status == ChatStatus::Truncated && json.content == partial);
  CHECK(client.chat(messages, options).empty());
  options.allow_partial = true;
  CHECK(client.chat(messages, options) == partial);
  CHECK(client.api_calls() == 5);
  CHECK(client.cache_hits() == 0);
  CHECK(std::filesystem::is_empty(cache.root()));
}

static void test_complete_json_cache() {
  const ame_test::TempDirectory cache;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  const std::string content = R"({"ok":true})";
  int calls = 0;
  client.set_transport([&calls, &content](const std::string&, std::string* out) {
    ++calls;
    *out = llm_response(content, "stop");
    return 200;
  });
  ChatOptions options;
  options.json = true;
  const std::vector<ChatMessage> messages{{"user", "complete JSON"}};
  const auto result = client.chat_result(messages, options);
  CHECK(result.status == ChatStatus::Complete && result.content == content);
  CHECK(client.chat(messages, options) == content);
  CHECK(calls == 1);
  CHECK(client.api_calls() == 1 && client.cache_hits() == 1);
}

static void test_partial_survives_empty_truncation() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string* out) {
    *out = llm_response(++calls == 1 ? "earlier partial" : "");
    return 200;
  });
  ChatOptions options;
  options.max_tokens = 16384;
  const auto result = client.chat_result({{"user", "empty truncation"}}, options);
  CHECK(result.status == ChatStatus::Truncated && result.content == "earlier partial");
  CHECK(calls == 2);
  CHECK(clock.waits.empty());
}

static void test_unknown_finish_preserves_partial() {
  const ame_test::TempDirectory cache;
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  client.set_retry_runtime(clock.runtime());
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string* out) {
    *out = ++calls == 1 ? llm_response("earlier partial")
                       : llm_response("unusable response", "content_filter");
    return 200;
  });
  ChatOptions options;
  options.max_tokens = 20000;
  const auto result = client.chat_result({{"user", "unknown finish reason"}}, options);
  CHECK(result.status == ChatStatus::Failed && result.content == "earlier partial");
  CHECK(calls == 2);
  CHECK(std::filesystem::is_empty(cache.root()));
}

static void test_empty_cache_is_miss() {
  const ame_test::TempDirectory cache;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string* out) {
    ++calls;
    *out = llm_response("complete", "stop");
    return 200;
  });
  const std::vector<ChatMessage> messages{{"user", "empty cache recovery"}};
  CHECK(client.chat(messages, {}) == "complete");
  for (const auto& entry : std::filesystem::directory_iterator(cache.root()))
    ame_test::write_text_file(entry.path().string(), "");
  const auto result = client.chat_result(messages, {});
  CHECK(result.status == ChatStatus::Complete && result.content == "complete");
  CHECK(calls == 2 && client.cache_hits() == 0);
}

static void test_concurrent_complete_cache() {
  const ame_test::TempDirectory cache;
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  client.set_retry_runtime(clock.runtime());
  std::mutex mutex;
  std::condition_variable both_entered;
  int calls = 0;
  const std::string first(65536, 'A');
  const std::string second(65536, 'B');
  client.set_transport([&mutex, &both_entered, &calls, &first, &second](const std::string&, std::string* out) {
    std::unique_lock<std::mutex> lock(mutex);
    const int index = ++calls;
    both_entered.notify_all();
    if (!both_entered.wait_for(lock, std::chrono::seconds{1}, [&calls] { return calls == 2; })) return 400;
    *out = llm_response(index == 1 ? first : second, "stop");
    return 200;
  });
  const ChatRequest request{{{"user", "same cache key"}}, {}};
  const auto results = client.chat_batch({request, request}, 2);
  CHECK(results.size() == 2);
  for (const auto& result : results) CHECK(result == first || result == second);
  CHECK(results.size() == 2 && results[0] != results[1]);
  const auto cached = client.chat_result(request.messages, request.opts);
  CHECK(cached.status == ChatStatus::Complete);
  CHECK(cached.content == first || cached.content == second);
  CHECK(calls == 2 && client.cache_hits() == 1);
  CHECK(std::distance(std::filesystem::directory_iterator(cache.root()),
                      std::filesystem::directory_iterator{}) == 1);
}

static void test_deadline_remaining_time() {
  VirtualRetryClock clock(Milliseconds{300});
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  std::vector<Milliseconds> remaining;
  client.set_timed_transport([&clock, &remaining](const std::string&, std::string* out,
                                                Milliseconds budget) {
    remaining.push_back(budget);
    clock.advance(Milliseconds{250});
    if (remaining.size() == 1) return 503;
    *out = llm_response("complete", "stop");
    return 200;
  });
  ChatOptions options;
  options.timeout = Milliseconds{1000};
  const auto result = client.chat_result({{"user", "remaining time"}}, options);
  CHECK(result.status == ChatStatus::Complete && result.content == "complete");
  CHECK(remaining == std::vector<Milliseconds>({Milliseconds{1000}, Milliseconds{450}}));
  CHECK(clock.waits == std::vector<Milliseconds>({Milliseconds{300}}));
  CHECK(client.api_calls() == 2);
}

static void test_deadline_caps_retry_wait() {
  VirtualRetryClock clock(Milliseconds{1000});
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  int calls = 0;
  client.set_transport([&clock, &calls](const std::string&, std::string*) {
    ++calls;
    clock.advance(Milliseconds{30});
    return 503;
  });
  ChatOptions options;
  options.timeout = Milliseconds{100};
  const auto result = client.chat_result({{"user", "bounded wait"}}, options);
  CHECK(result.status == ChatStatus::DeadlineExceeded && result.content.empty());
  CHECK(calls == 1);
  CHECK(clock.delay_attempts == std::vector<int>({1}));
  CHECK(clock.waits == std::vector<Milliseconds>({Milliseconds{70}}));
}

static void test_deadline_expired_before_wait() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  client.set_retry_runtime(clock.runtime());
  int calls = 0;
  client.set_transport([&clock, &calls](const std::string&, std::string*) {
    ++calls;
    clock.advance(Milliseconds{100});
    return 503;
  });
  ChatOptions options;
  options.timeout = Milliseconds{100};
  const auto result = client.chat_result({{"user", "expired transport"}}, options);
  CHECK(result.status == ChatStatus::DeadlineExceeded && result.content.empty());
  CHECK(calls == 1);
  CHECK(clock.waits.empty());
  CHECK(clock.delay_attempts.empty());
}

static void test_deadline_expired_during_delay_sampling() {
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  auto runtime = clock.runtime();
  int delay_calls = 0;
  runtime.delay = [&clock, &delay_calls](int) {
    ++delay_calls;
    clock.advance(Milliseconds{100});
    return Milliseconds{1000};
  };
  client.set_retry_runtime(std::move(runtime));
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string*) { ++calls; return 503; });
  ChatOptions options;
  options.timeout = Milliseconds{100};
  const auto result = client.chat_result({{"user", "expired during delay sampling"}}, options);
  CHECK(result.status == ChatStatus::DeadlineExceeded && result.content.empty());
  CHECK(calls == 1 && delay_calls == 1);
  CHECK(clock.waits.empty());
}

static void test_deadline_rejects_late_completion() {
  const ame_test::TempDirectory cache;
  VirtualRetryClock clock;
  DeepSeekClient client("https://example.invalid", "k", "m", cache.root().string());
  client.set_retry_runtime(clock.runtime());
  std::vector<Milliseconds> remaining;
  client.set_timed_transport([&clock, &remaining](const std::string&, std::string* out,
                                                Milliseconds budget) {
    remaining.push_back(budget);
    const bool first = remaining.size() == 1;
    clock.advance(first ? Milliseconds{10} : Milliseconds{90});
    *out = first ? llm_response("earlier partial") : llm_response("late completion", "stop");
    return 200;
  });
  ChatOptions options;
  options.max_tokens = 20000;
  options.timeout = Milliseconds{100};
  const auto result = client.chat_result({{"user", "late response"}}, options);
  CHECK(result.status == ChatStatus::DeadlineExceeded);
  CHECK(result.content == "earlier partial");
  CHECK(remaining == std::vector<Milliseconds>({Milliseconds{100}, Milliseconds{90}}));
  CHECK(client.api_calls() == 2);
  CHECK(clock.waits.empty());
  CHECK(std::filesystem::is_empty(cache.root()));
}

static void test_deadline_before_first_transport() {
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  int clock_reads = 0;
  int calls = 0;
  client.set_retry_runtime({[&clock_reads] {
                              return LlmRetryRuntime::Clock::time_point{} +
                                     Milliseconds{100 * clock_reads++};
                            },
                            [](Milliseconds) {}, [](int) { return Milliseconds{0}; }});
  client.set_transport([&calls](const std::string&, std::string*) { ++calls; return 503; });
  ChatOptions options;
  options.timeout = Milliseconds{50};
  const auto result = client.chat_result({{"user", "expired before dispatch"}}, options);
  CHECK(result.status == ChatStatus::DeadlineExceeded && result.content.empty());
  CHECK(calls == 0 && client.api_calls() == 0);
}

static void test_invalid_request_budgets() {
  DeepSeekClient client("https://example.invalid", "k", "m", "");
  int calls = 0;
  client.set_transport([&calls](const std::string&, std::string*) { ++calls; return 400; });
  ChatOptions options;
  for (int tokens : {0, -1, 32769, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    options.max_tokens = tokens;
    const auto result = client.chat_result({{"user", "invalid tokens"}}, options);
    CHECK(result.status == ChatStatus::Failed && result.content.empty());
  }
  options.max_tokens = 1024;
  for (auto timeout : {Milliseconds{0}, Milliseconds{-1}, Milliseconds::min()}) {
    options.timeout = timeout;
    const auto result = client.chat_result({{"user", "invalid timeout"}}, options);
    CHECK(result.status == ChatStatus::Failed && result.content.empty());
  }
  CHECK(calls == 0 && client.api_calls() == 0);
}

static void test_llm_extractor() {
  FakeLlmClient fake;
  fake.default_response = "not json at all";  // 触发回退
  fake.rules.push_back(
      {"Mochi",
       R"([{"type":"life","entities":[{"surface":"Mochi","role":"客体","type":"object"}],"keywords":[{"word":"adopted","category":"action"},{"word":"cat","category":"concept"}]}])"});
  RuleExtractor rule;
  LlmExtractor ext(&fake, &rule);

  // LLM 正常路径：JSON 数组解析
  auto v = ext.extract_session({{"Alice", "I adopted a cat named Mochi."}});
  CHECK(v.size() == 1);
  CHECK(v[0].event.text == "I adopted a cat named Mochi.");
  bool has_mochi = false, has_alice = false;
  for (auto& e : v[0].entities) {
    if (e.surface == "Mochi") has_mochi = true;
    if (e.surface == "Alice") has_alice = true;  // speaker 主体实体兜底
  }
  CHECK(has_mochi && has_alice);
  CHECK(v[0].keywords.size() == 2);
  CHECK(v[0].event.type == "life");

  // 回退路径：LLM 输出不合法 → 规则提取器
  auto v2 = ext.extract_session({{"Bob", "老张在朝阳公园跑步"}});
  CHECK(v2.size() == 1);
  bool rule_hit = false;
  for (auto& k : v2[0].keywords)
    if (k.word == "跑步") rule_hit = true;  // 规则词典命中
  CHECK(rule_hit);
}

static void test_answerer_and_judge() {
  FakeLlmClient fake;
  fake.rules.push_back({"favorite restaurant", "NO INFORMATION"});
  fake.rules.push_back({"cat", "Mochi"});
  LlmAnswerer ans(&fake);
  auto req = ans.build_request("What is Alice's favorite restaurant?", "2023-09-01",
                               {"[ts=1700000000] I adopted a cat named Mochi."});
  std::string resp = fake.chat(req.messages, req.opts);
  CHECK(LlmAnswerer::is_abstain(resp));
  auto req2 = ans.build_request("What is the name of the cat?", "", {"...Mochi..."});
  CHECK(!LlmAnswerer::is_abstain(fake.chat(req2.messages, req2.opts)));

  // judge yes/no 解析
  CHECK(LlmJudge::parse_yes_no("yes"));
  CHECK(LlmJudge::parse_yes_no(" Yes.\n"));
  CHECK(!LlmJudge::parse_yes_no("no"));
  CHECK(!LlmJudge::parse_yes_no(""));

  FakeLlmClient fake2;
  fake2.default_response = "[1, 0.5, 0]";
  LlmJudge judge(&fake2);
  auto scores = judge.judge("q", "a", {"n1", "n2", "n3"});
  CHECK(scores.size() == 3);
  CHECK_NEAR(scores[0], 1.0, 1e-9);
  CHECK_NEAR(scores[1], 0.5, 1e-9);
  CHECK_NEAR(scores[2], 0.0, 1e-9);

  FakeLlmClient fake3;
  fake3.default_response = "yes";
  LlmJudge judge2(&fake3);
  CHECK(judge2.correctness("q", "gold", "pred") == 1.0);
}

// 夹具级端到端：fake client → llm 提取摄入 → llm 答题 → llm 判分
static void test_fake_pipeline_e2e() {
  FakeLlmClient fake;
  fake.rules.push_back(
      {"Mochi",
       R"([{"type":"life","entities":[],"keywords":[{"word":"cat","category":"concept"},{"word":"mochi","category":"attr"}]}])"});
  RuleExtractor rule;
  LlmExtractor ext(&fake, &rule);

  Engine e;
  auto exts = ext.extract_session({{"Alice", "I adopted a cat named Mochi."}});
  auto wr = e.remember_turn_ex(std::move(exts[0]), "Alice", 1700000000, "D1:1", "s1");
  CHECK(wr.code == Err::Ok);
  // LLM 提取的关键词已落图
  CHECK(e.keywords().find("mochi") != nullptr);

  // fake 答题 + 判分（judge 用独立的 fake，避免规则串扰）
  FakeLlmClient fake2;
  fake2.rules.push_back({"cat", "The cat is Mochi"});
  FakeLlmClient fake3;
  fake3.default_response = "yes";
  LlmAnswerer ans(&fake2);
  LlmJudge judge(&fake3);
  auto req = ans.build_request("What is the name of Alice's cat?", "", {});
  std::string pred = fake2.chat(req.messages, req.opts);
  CHECK(!LlmAnswerer::is_abstain(pred));
  CHECK(judge.correctness("What is the name of Alice's cat?", "Mochi", pred) == 1.0);
}

// 证据整合器 mock 测试：分块摘要 + 合并轮 + 失败回退（不打真实 API）
// 注：digest 功能经两轮 A/B 判负已移除（README「已否证的假设」）。
// 两阶段聚合作答 mock 测试：逐项 NONE 过滤、空列表弃答、聚合输入只含事实列表
static void test_two_stage_aggregate() {
  // 模式识别
  CHECK(LlmAnswerer::is_aggregate_question("How many trips did I take?"));
  CHECK(LlmAnswerer::is_aggregate_question("总共去了几个城市"));
  CHECK(!LlmAnswerer::is_aggregate_question("Where does Alice live?"));
  // NONE 判定
  CHECK(LlmAnswerer::is_none("NONE"));
  CHECK(LlmAnswerer::is_none("none."));
  CHECK(!LlmAnswerer::is_none("她搬到了 Portland"));
  // 阶段一请求：保留思考（关思考会过度 NONE）+ 含问题与证据
  auto r1 = LlmAnswerer::build_extract_request("我去了几个城市？", "[2023-01-01] 搬到 Seattle");
  CHECK(!r1.opts.disable_thinking);
  CHECK(r1.messages[1].content.find("搬到 Seattle") != std::string::npos);
  // 阶段二请求：输入只含事实列表，不含原文长片段
  FakeLlmClient fake;
  LlmAnswerer ans(&fake);
  auto r2 = ans.build_aggregate_request("我去了几个城市？", "", {"[E1] 搬到 Seattle",
                                                               "[E2] 搬到 Portland"});
  CHECK(r2.messages[1].content.find("搬到 Portland") != std::string::npos);
  CHECK(r2.messages[1].content.find("问题: 我去了几个城市？") != std::string::npos);
  CHECK(r2.messages[1].content.find("证据片段") == std::string::npos);  // 不走单阶段原文通道
}

// 批量提取 mock 测试：批量请求构造、解析（部分 NONE/编号错位/越界）、空列表判定
static void test_extract_batch() {
  // 批量请求：编号 + 保留思考
  auto req = LlmAnswerer::build_extract_batch_request(
      "我去了几个城市？", {"[2023-01-01] 搬到 Seattle", "[2023-02-01] 爬山", "[2023-03-01] 搬到 Portland"});
  CHECK(req.messages[1].content.find("[E2]") != std::string::npos);
  CHECK(!req.opts.disable_thinking);
  // 解析：E1 事实、E2 NONE、E4 越界、E1 重复取首次
  auto facts = LlmAnswerer::parse_extract_batch(
      "[E1]: 搬到 Seattle\n[E2]: NONE\n[E3]: 搬到 Portland\n[E4]: 越界\n[E1]: 重复忽略\n", 3);
  CHECK(facts.size() == 2);
  CHECK(facts[0].first == 0 && facts[1].first == 2);
  // 全 NONE → 空列表
  CHECK(LlmAnswerer::parse_extract_batch("[E1]: NONE\n[E2]: none\n", 2).empty());
  // 乱序/杂行宽容
  auto f2 = LlmAnswerer::parse_extract_batch("前言废话\n[E2]: 只收 E2\n", 2);
  CHECK(f2.size() == 1 && f2[0].first == 1);
  CHECK(LlmAnswerer::parse_extract_batch(
      "[E1junk]: invalid\n[E999999999999999999999]: overflow\n[E-1]: negative\n", 3).empty());
  CHECK(LlmAnswerer::parse_extract_batch("[E1]: valid\n", -1).empty());
}

// 唯一临时目录隔离词典检查点；所有 LLM 调用均为内存 fake。
struct LexiconFiles {
  const ame_test::TempDirectory directory;
  std::string path(const std::string& name) const { return directory.path(name); }
  void write(const std::string& name, const Json& value) const {
    ame_test::write_text_file(path(name), value.dump());
  }
  Json read(const std::string& name) const {
    std::ifstream input(path(name));
    std::stringstream contents;
    contents << input.rdbuf();
    bool ok = false;
    Json value = Json::parse(contents.str(), &ok);
    CHECK(ok);
    return value;
  }
};

static Json lexicon_entry(const std::string& word, int pairs) {
  Json entry = Json::object();
  entry["word"] = Json::string(word);
  Json strong = Json::array();
  for (int i = 0; i < pairs; ++i) strong.push(Json::string("syn-" + std::to_string(i)));
  entry["syn_strong"] = std::move(strong);
  return entry;
}

static Json lexicon_input(int pairs, bool completed_prefix = false) {
  Json root = Json::object();
  Json entries = Json::array();
  if (completed_prefix) entries.push(lexicon_entry("finished", 1));
  entries.push(lexicon_entry("anchor", pairs));
  root["entries"] = std::move(entries);
  return root;
}

static Json find_lexicon_entry(const Json& root, const std::string& word) {
  const Json* entries = root.find("entries");
  if (!entries) return {};
  for (const auto& entry : entries->arr) {
    const Json* candidate = entry.find("word");
    if (candidate && candidate->as_str() == word) return entry;
  }
  return {};
}

static void test_lexicon_cross_chunk() {
  LexiconFiles files;
  files.write("input.json", lexicon_input(101));
  FakeLlmClient score, validate;
  score.default_response = "[]";
  validate.default_response = "[]";
  CHECK(score_word_sims(score, files.path("input.json"), files.path("scores.json"), 1, 1) == 101);
  Json scored = files.read("scores.json");
  CHECK(scored["sims"]["anchor"].obj.size() == 101);
  CHECK_NEAR(scored["sims"]["anchor"]["syn-100"].as_num(), 0.5, 1e-9);
  CHECK(score.received.size() == 101);
  CHECK(validate_lexicon_json(validate, files.path("input.json"), files.path("validated.json"), 1, 1) == 101);
  Json validated = files.read("validated.json");
  CHECK(validated["entries"].arr.size() == 1);
  CHECK(find_lexicon_entry(validated, "anchor")["syn_strong"].arr.empty());
  CHECK(validate.received.size() == 101);
  // 完整检查点再次运行不发请求。
  CHECK(score_word_sims(score, files.path("input.json"), files.path("scores.json"), 1, 1) == 0);
  CHECK(validate_lexicon_json(validate, files.path("input.json"), files.path("validated.json"), 1, 1) == 0);
  CHECK(score.received.size() == 101 && validate.received.size() == 101);
}

struct LexiconInterrupted final : std::exception {
  const char* what() const noexcept override { return "Lexicon test interrupted at a chunk boundary"; }
};
class InterruptingLexiconClient : public FakeLlmClient {
 public:
  int batches = 0;
  std::vector<std::string> chat_batch(const std::vector<ChatRequest>& requests, int concurrency) override {
    if (++batches == 2) throw LexiconInterrupted{};
    return ILlmClient::chat_batch(requests, concurrency);
  }
};

using LexiconOperation = int (*)(ILlmClient&, const std::string&, const std::string&, int, int);
static void interrupt_after_first_chunk(LexiconOperation operation, const LexiconFiles& files) {
  InterruptingLexiconClient client;
  client.default_response = "invalid JSON";  // validate 保留，score 采用中性分数。
  bool interrupted = false;
  try {
    operation(client, files.path("input.json"), files.path("checkpoint.json"), 1, 1);
  } catch (const LexiconInterrupted&) {
    interrupted = true;
  }
  CHECK(interrupted);
  CHECK(client.received.size() == 100);
}

static void test_validate_word_checkpoint_resume() {
  LexiconFiles files;
  files.write("input.json", lexicon_input(101, true));
  interrupt_after_first_chunk(validate_lexicon_json, files);
  Json partial = files.read("checkpoint.json");
  CHECK(partial["entries"].arr.size() == 1);
  CHECK(!find_lexicon_entry(partial, "finished").is_null());
  CHECK(find_lexicon_entry(partial, "anchor").is_null());
  FakeLlmClient resumed;
  resumed.default_response = "invalid JSON";
  CHECK(validate_lexicon_json(resumed, files.path("input.json"), files.path("checkpoint.json"), 1, 1) == 101);
  Json complete = files.read("checkpoint.json");
  CHECK(complete["entries"].arr.size() == 2);
  CHECK(find_lexicon_entry(complete, "anchor")["syn_strong"].arr.size() == 101);
  CHECK(find_lexicon_entry(complete, "finished")["syn_strong"].arr.size() == 1);
  CHECK(resumed.received.size() == 101);
}

static void test_score_word_checkpoint_resume() {
  LexiconFiles files;
  files.write("input.json", lexicon_input(101, true));
  interrupt_after_first_chunk(score_word_sims, files);
  Json partial = files.read("checkpoint.json");
  CHECK(partial["sims"].obj.size() == 1);
  CHECK(partial["sims"].find("finished") != nullptr);
  CHECK(partial["sims"].find("anchor") == nullptr);
  FakeLlmClient resumed;
  resumed.default_response = "[]";
  CHECK(score_word_sims(resumed, files.path("input.json"), files.path("checkpoint.json"), 1, 1) == 101);
  Json complete = files.read("checkpoint.json");
  CHECK(complete["sims"]["anchor"].obj.size() == 101);
  CHECK_NEAR(complete["sims"]["anchor"]["syn-100"].as_num(), 0.5, 1e-9);
  CHECK(complete["sims"]["finished"].obj.size() == 1);
  CHECK(resumed.received.size() == 101);
}

static void test_legacy_lexicon_checkpoint_repair() {
  LexiconFiles files;
  files.write("input.json", lexicon_input(101));
  Json old_scores = Json::object();
  for (int i = 0; i < 100; ++i) old_scores["sims"]["anchor"]["syn-" + std::to_string(i)] = Json::number(0.7);
  files.write("scores.json", old_scores);
  Json old_validated = lexicon_input(100);
  old_validated["entries"].push(lexicon_entry("anchor", 100));
  old_validated["entries"].push(lexicon_entry("unrelated-history", 1));
  old_validated["validated"] = Json::boolean(true);
  files.write("validated.json", old_validated);
  FakeLlmClient score, validate;
  score.default_response = "[]";
  validate.default_response = "invalid JSON";
  CHECK(score_word_sims(score, files.path("input.json"), files.path("scores.json"), 1, 1) == 101);
  CHECK(files.read("scores.json")["sims"]["anchor"].obj.size() == 101);
  CHECK(validate_lexicon_json(validate, files.path("input.json"), files.path("validated.json"), 1, 1) == 101);
  Json repaired = files.read("validated.json");
  CHECK(repaired["entries"].arr.size() == 2);
  CHECK(find_lexicon_entry(repaired, "anchor")["syn_strong"].arr.size() == 101);
  CHECK(find_lexicon_entry(repaired, "unrelated-history")["syn_strong"].arr.size() == 1);
  CHECK(repaired["checkpoint_version"].as_num() == 2);
}

static void test_validate_retry_and_fallback() {
  LexiconFiles files;
  files.write("input.json", lexicon_input(5));
  FakeLlmClient fake;
  fake.default_response = "[]";
  fake.rules = {{"anchor|syn-0\n", ""}, {"anchor|syn-1\n", "invalid"},
                {"anchor|syn-2\n", R"([{"a":"anchor","b":"syn-2","ok":true}])"},
                {"anchor|syn-3\n", R"([{"a":"anchor","b":"syn-3","ok":false}])"}};
  CHECK(validate_lexicon_json(fake, files.path("input.json"), files.path("out.json"), 1, 1) == 5);
  Json result = find_lexicon_entry(files.read("out.json"), "anchor");
  CHECK(result["syn_strong"].arr.size() == 3);
  CHECK(result["syn_strong"].dump() == R"(["syn-0","syn-1","syn-2"])");
  CHECK(fake.received.size() == 6);  // 仅空响应重试一次，malformed 不重试。
}

static void test_validate_without_strong_candidates() {
  LexiconFiles files;
  Json input = lexicon_input(0);
  input["entries"].arr[0]["syn_near"] = Json::parse(R"([{"word":"nearby","score":0.6}])");
  input["entries"].arr[0]["antonyms"] = Json::parse(R"(["opposite"])");
  files.write("input.json", input);
  FakeLlmClient fake;
  CHECK(validate_lexicon_json(fake, files.path("input.json"), files.path("out.json"), 1, 1) == 0);
  Json output = files.read("out.json");
  CHECK(output["entries"].dump() == input["entries"].dump());
  CHECK(fake.received.empty());
}

static void test_score_retry_defaults_and_deduplication() {
  LexiconFiles files;
  Json input = lexicon_input(5);
  input["entries"].arr[0]["syn_strong"].push(Json::string("syn-0"));
  input["entries"].arr[0]["syn_near"] = Json::parse(R"([{"word":"syn-2"},{"word":"syn-5"}])");
  files.write("input.json", input);
  FakeLlmClient fake;
  fake.default_response = "[]";
  fake.rules = {{"anchor|syn-0\n", ""}, {"anchor|syn-1\n", "invalid"},
                {"anchor|syn-2\n", R"([{"a":"anchor","b":"syn-2","score":9}])"},
                {"anchor|syn-3\n", R"([{"a":"anchor","b":"syn-3","score":"invalid"}])"}};
  CHECK(score_word_sims(fake, files.path("input.json"), files.path("out.json"), 1, 1) == 6);
  Json row = files.read("out.json")["sims"]["anchor"];
  CHECK(row.obj.size() == 6);
  CHECK_NEAR(row["syn-2"].as_num(), 0.9, 1e-9);
  for (const char* other : {"syn-0", "syn-1", "syn-3", "syn-4", "syn-5"})
    CHECK_NEAR(row[other].as_num(), 0.5, 1e-9);
  CHECK(fake.received.size() == 7);
}

class RetryOnceLexiconClient : public FakeLlmClient {
 public:
  int calls = 0;
  std::string chat(const std::vector<ChatMessage>& messages, const ChatOptions& options) override {
    if (++calls == 1) return "";
    return FakeLlmClient::chat(messages, options);
  }
};

static void test_build_lexicon_resume_and_legacy_fields() {
  LexiconFiles files;
  files.write("vocab.json", Json::parse(R"({"conv_vocab_top":[["walk",5],["run",3],["a",90],["can't",80]]})"));
  Json previous = Json::object();
  previous["entries"] = Json::array();
  previous["entries"].push(lexicon_entry("walk", 1));
  files.write("out.json", previous);
  RetryOnceLexiconClient fake;
  fake.default_response = R"({"run":{"synonyms":["running"],"related":[{"word":"jog"}],"antonyms":["stop"]}})";
  CHECK(build_lexicon_json(fake, {files.path("vocab.json")}, files.path("out.json"), 10, 1, 1) == 1);
  Json result = files.read("out.json");
  CHECK(result["entries"].arr.size() == 2);
  Json run = find_lexicon_entry(result, "run");
  CHECK(run["syn_strong"].dump() == R"(["running"])");
  CHECK(run["antonyms"].dump() == R"(["stop"])");
  CHECK(run["syn_near"].dump() == R"([{"word":"jog","score":0.5}])");
  CHECK(fake.calls == 2);
  CHECK(build_lexicon_json(fake, {files.path("vocab.json")}, files.path("out.json"), 10, 1, 1) == 0);
  CHECK(fake.calls == 2);
  CHECK(build_lexicon_json(fake, {files.path("vocab.json")}, files.path("out.json"), 10, 0, 1) == -1);
  CHECK(validate_lexicon_json(fake, files.path("out.json"), files.path("unused.json"), 0, 1) == -1);
  CHECK(score_word_sims(fake, files.path("out.json"), files.path("unused.json"), 0, 1) == -1);
}

static void test_extraction_field_defaults() {
  FakeLlmClient fake;
  fake.default_response = R"([{"type":null,"entities":[{"surface":"Alice"},{"surface":""},{"surface":7},{"surface":"X","role":"","type":null}],"keywords":[{"word":"unknown","category":"bad"},{"word":"known","category":"action"},{"category":"attr"}],"attributes":[{"entity":"Alice","slot":"residence","value":"Paris"},{"entity":"Alice","value":"Paris"},{"entity":"Alice","slot":"age","value":7}]}])";
  RuleExtractor fallback;
  LlmExtractor extractor(&fake, &fallback);
  const auto result = extractor.extract_session({{"", "original text"}});
  CHECK(result.size() == 1);
  if (result.empty()) return;
  const auto& item = result.front();
  CHECK(item.event.type == "generic" && item.event.text == "original text");
  CHECK(item.entities.size() == 2);
  if (item.entities.size() == 2) {
    CHECK(item.entities[0].role == "人物" && item.entities[0].entity_type == "person");
    CHECK(item.entities[1].role.empty() && item.entities[1].entity_type == "person");
  }
  CHECK(item.keywords.size() == 2);
  if (item.keywords.size() == 2) {
    CHECK(item.keywords[0].category == "attr");
    CHECK(item.keywords[1].category == "action");
  }
  CHECK(item.attributes.size() == 1);
}

int main() {
  test_retry_backoff();
  test_retry_entropy_failure();
  test_retry_exhaustion();
  test_client_cache_and_retry();
  test_chat_batch();
  test_chat_batch_retries();
  test_token_expansion_cap();
  test_default_token_expansion();
  test_expansion_with_transient_failures();
  test_maximum_expansion_budget();
  test_transient_budget_preserves_partial();
  test_partial_cache_and_json_policy();
  test_complete_json_cache();
  test_partial_survives_empty_truncation();
  test_unknown_finish_preserves_partial();
  test_empty_cache_is_miss();
  test_concurrent_complete_cache();
  test_deadline_remaining_time();
  test_deadline_caps_retry_wait();
  test_deadline_expired_before_wait();
  test_deadline_expired_during_delay_sampling();
  test_deadline_rejects_late_completion();
  test_deadline_before_first_transport();
  test_invalid_request_budgets();
  test_llm_extractor();
  test_answerer_and_judge();
  test_fake_pipeline_e2e();
  test_two_stage_aggregate();
  test_extract_batch();
  test_lexicon_cross_chunk();
  test_validate_word_checkpoint_resume();
  test_score_word_checkpoint_resume();
  test_legacy_lexicon_checkpoint_repair();
  test_validate_retry_and_fallback();
  test_validate_without_strong_candidates();
  test_score_retry_defaults_and_deduplication();
  test_build_lexicon_resume_and_legacy_fields();
  test_extraction_field_defaults();
  // 集成测试（真实 API）默认跳过
  if (std::getenv("AME_RUN_LLM_IT")) {
    const ame_test::TempDirectory cache;
    auto client = DeepSeekClient::from_config("bench/config/deepseek.local.json",
                                              "deepseek-v4-flash", cache.root().string());
    if (client) {
      std::string r = client->chat({{"user", "只回答 ok"}}, {0.0, 8, false});
      CHECK(!r.empty());
    }
  }
  return AME_TEST_REPORT();
}
