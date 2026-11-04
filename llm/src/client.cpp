/*
 * 所属模块：llm 接入层——DeepSeekClient 实现。
 * 本文件主体思路：
 *   传输：curl 子进程（请求体与含鉴权的 curl 配置写入临时文件，避免 key 出现在
 *     进程命令行）；HTTP 状态码经 -w 取回；
 *   缓存：请求哈希 → cache_dir/<hash>.txt，命中直接返回（断点续跑零重复扣费）；
 *   Retries: separate transient-failure and token-expansion limits share one deadline.
 *   并发：chat_batch 用固定线程池 + 原子工作索引。
 * 关键算法/数据结构：FNV-1a 64 位哈希做缓存键；临时文件用 mkostemp + CLOEXEC。
 * 依赖关系：依赖 core(json.h) 做响应解析；被 components 与 bench 依赖。
 * 安全约束：api_key 只写入临时 curl 配置文件（0600 权限），不打印、不拼进命令行字符串。
 */
#include "ame/llm/llm.h"
#include "curl_transport.h"
#include "retry_backoff.h"

#include "ame/core/json.h"
#include "ame/core/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace ame {
namespace {

constexpr int kMaxOutputTokens = 32768;
constexpr int kTokenGrowthFactor = 2;
constexpr int kMaxTokenExpansions = 15;
constexpr int kMaxTotalRequests = detail::kMaxTransientFailures + kMaxTokenExpansions;
constexpr auto kMaxTransferTime = std::chrono::seconds{120};

struct Completion {
  bool valid = false;
  bool truncated = false;
  std::string content;
};

Completion parse_completion(const std::string& response) {
  bool ok = false;
  const Json root = Json::parse(response, &ok);
  const Json* choices = ok ? root.find("choices") : nullptr;
  if (!choices || choices->arr.empty()) return {};
  const Json& choice = choices->arr.front();
  const Json* finish = choice.find("finish_reason");
  const std::string reason = finish ? finish->as_str() : "";
  if (!reason.empty() && reason != "stop" && reason != "length") return {};
  const Json* message = choice.find("message");
  const Json* content = message ? message->find("content") : nullptr;
  return {true, reason == "length", content ? content->as_str() : ""};
}

class RetryBudget {
 public:
  RetryBudget(const LlmRetryRuntime& runtime, std::chrono::milliseconds limit)
      : runtime_(runtime), started_(runtime.now()), limit_(limit) {}

  std::chrono::milliseconds remaining() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(runtime_.now() - started_);
    if (elapsed >= limit_) return std::chrono::milliseconds::zero();
    return limit_ - std::max(elapsed, std::chrono::milliseconds::zero());
  }

  bool wait(int failure) const {
    if (remaining() <= std::chrono::milliseconds::zero()) return false;
    if (failure == 0) return true;
    const auto requested = runtime_.delay(failure);
    const auto left = remaining();
    if (left <= std::chrono::milliseconds::zero()) return false;
    const auto delay = std::clamp(requested, std::chrono::milliseconds::zero(), left);
    if (delay > std::chrono::milliseconds::zero()) runtime_.sleep(delay);
    return remaining() > std::chrono::milliseconds::zero();
  }

 private:
  const LlmRetryRuntime& runtime_;
  const LlmRetryRuntime::Clock::time_point started_;
  const std::chrono::milliseconds limit_;
};

struct RetryProgress {
  int failures = 0;
  int expansions = 0;
  int pending_failure = 0;
  ChatResult partial;
};

bool expand_truncated(const Completion& completion, ChatOptions& options, RetryProgress& progress) {
  if (!completion.content.empty()) progress.partial.content = completion.content;
  progress.partial.status = ChatStatus::Truncated;
  if (options.max_tokens >= kMaxOutputTokens || progress.expansions >= kMaxTokenExpansions) return false;
  options.max_tokens = std::min(options.max_tokens * kTokenGrowthFactor, kMaxOutputTokens);
  ++progress.expansions;
  AME_LOGW("llm: truncated response, retrying with max_tokens=", options.max_tokens);
  return true;
}

ChatResult deadline_result(ChatResult partial) {
  partial.status = ChatStatus::DeadlineExceeded;
  return partial;
}

ChatResult failed_result(ChatResult partial) {
  partial.status = ChatStatus::Failed;
  return partial;
}

class CacheFile {
 public:
  explicit CacheFile(const std::string& target)
      : path_(target + ".XXXXXX"), descriptor_(::mkostemp(path_.data(), O_CLOEXEC)) {}
  ~CacheFile() {
    if (descriptor_ >= 0) ::close(descriptor_);
    if (created_) ::unlink(path_.c_str());
  }
  CacheFile(const CacheFile&) = delete;
  CacheFile& operator=(const CacheFile&) = delete;
  CacheFile(CacheFile&&) = delete;
  CacheFile& operator=(CacheFile&&) = delete;

  void publish(const std::string& target, const std::string& content) {
    if (descriptor_ < 0 || !write_all(content)) return;
    const int closed = ::close(descriptor_);
    descriptor_ = -1;
    if (closed == 0 && std::rename(path_.c_str(), target.c_str()) == 0) created_ = false;
  }

 private:
  bool write_all(const std::string& content) const {
    size_t offset = 0;
    while (offset < content.size()) {
      const auto count = ::write(descriptor_, content.data() + offset, content.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += static_cast<size_t>(count);
    }
    return true;
  }

  std::string path_;
  int descriptor_;
  bool created_ = descriptor_ >= 0;
};

bool retryable_status(int code) {
  return code == 0 || code == 429 || (code >= 500 && code <= 599);
}

bool valid_options(const ChatOptions& options) {
  return options.max_tokens > 0 && options.max_tokens <= kMaxOutputTokens
      && options.timeout > std::chrono::milliseconds::zero();
}

}  // namespace

// FNV-1a 64 位（缓存键用，进程间稳定）
static uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

DeepSeekClient::DeepSeekClient(std::string base_url, std::string api_key, std::string model,
                               std::string cache_dir)
    : base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      model_(std::move(model)),
      cache_dir_(std::move(cache_dir)) {
  transport_ = [this](const std::string& body, std::string* out, std::chrono::milliseconds remaining) {
    return detail::curl_transport(base_url_, api_key_, body, out, remaining);
  };
  runtime_ = {[] { return LlmRetryRuntime::Clock::now(); },
              [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); },
              [](int failure) { return detail::retry_backoff(failure); }};
}

void DeepSeekClient::set_transport(LlmTransport transport) {
  if (!transport) throw std::invalid_argument("Transport must be callable");
  transport_ = [transport = std::move(transport)](const std::string& body, std::string* out,
                                                std::chrono::milliseconds) {
    return transport(body, out);
  };
}

void DeepSeekClient::set_timed_transport(LlmTimedTransport transport) {
  if (!transport) throw std::invalid_argument("Timed transport must be callable");
  transport_ = std::move(transport);
}

void DeepSeekClient::set_retry_runtime(LlmRetryRuntime runtime) {
  if (!runtime.now || !runtime.sleep || !runtime.delay)
    throw std::invalid_argument("Retry runtime requires a clock, sleeper and delay source");
  runtime_ = std::move(runtime);
}

// cache_key 伪代码：拼接 模型名 + 各消息(role:content) + 选项 → FNV-1a 十六进制。
std::string DeepSeekClient::cache_key(const std::vector<ChatMessage>& messages,
                                      const ChatOptions& opts) const {
  // Older cache entries may contain unmarked partial responses.
  std::string s = "complete-v2\n" + model_;
  for (auto& m : messages) { s += "\n" + m.role + ":" + m.content; }
  s += "\nT" + std::to_string(opts.temperature) + "M" + std::to_string(opts.max_tokens) +
       (opts.json ? "J" : "") + (opts.disable_thinking ? "D" : "");
  std::ostringstream hex;
  hex.imbue(std::locale::classic());
  hex << std::hex << std::setfill('0') << std::setw(16) << fnv64(s);
  return hex.str();
}

// build_body 伪代码：用 core Json 组装 OpenAI 兼容请求体（model/messages/温度/max_tokens，
//   json 模式附加 response_format，disable_thinking 时附加 thinking:{type:disabled}）。
std::string DeepSeekClient::build_body(const std::vector<ChatMessage>& messages,
                                       const ChatOptions& opts) const {
  Json root = Json::object();
  root["model"] = Json::string(model_);
  Json ms = Json::array();
  for (auto& m : messages) {
    Json jm = Json::object();
    jm["role"] = Json::string(m.role);
    jm["content"] = Json::string(m.content);
    ms.push(std::move(jm));
  }
  root["messages"] = std::move(ms);
  root["temperature"] = Json::number(opts.temperature);
  root["max_tokens"] = Json::number(opts.max_tokens);
  if (opts.json) {
    Json rf = Json::object();
    rf["type"] = Json::string("json_object");
    root["response_format"] = std::move(rf);
  }
  if (opts.disable_thinking) {
    // deepseek-v4 推理模型：关思考（实测有效，避免 reasoning 烧光 max_tokens）
    Json th = Json::object();
    th["type"] = Json::string("disabled");
    root["thinking"] = std::move(th);
  }
  return root.dump();
}

// Token growth and transient failures have separate limits and one time budget.
ChatResult DeepSeekClient::call_api(const std::vector<ChatMessage>& messages, ChatOptions opts) {
  const RetryBudget budget(runtime_, opts.timeout);
  RetryProgress progress;
  for (int request = 0; request < kMaxTotalRequests; ++request) {
    if (!budget.wait(progress.pending_failure)) return deadline_result(std::move(progress.partial));
    progress.pending_failure = 0;
    const std::string body = build_body(messages, opts);
    const auto remaining = std::min(budget.remaining(),
                                    std::chrono::duration_cast<std::chrono::milliseconds>(kMaxTransferTime));
    if (remaining <= std::chrono::milliseconds::zero()) return deadline_result(std::move(progress.partial));
    std::string out;
    const int code = transport_(body, &out, remaining);
    ++api_calls_;
    if (budget.remaining() <= std::chrono::milliseconds::zero())
      return deadline_result(std::move(progress.partial));
    if (code == 200) {
      const Completion completion = parse_completion(out);
      if (budget.remaining() <= std::chrono::milliseconds::zero())
        return deadline_result(std::move(progress.partial));
      if (!completion.valid) return failed_result(std::move(progress.partial));
      if (!completion.truncated) return {completion.content, ChatStatus::Complete};
      if (!expand_truncated(completion, opts, progress)) return progress.partial;
      continue;
    }
    if (!retryable_status(code)) {
      AME_LOGE("llm: HTTP ", code, " non-retryable error, body_bytes=", out.size());
      return failed_result(std::move(progress.partial));
    }
    ++progress.failures;
    if (progress.failures >= detail::kMaxTransientFailures) return failed_result(std::move(progress.partial));
    progress.pending_failure = progress.failures;
    AME_LOGW("llm: HTTP ", code, ", transient failure ", progress.failures);
  }
  return failed_result(std::move(progress.partial));
}

std::string DeepSeekClient::chat(const std::vector<ChatMessage>& messages, const ChatOptions& opts) {
  ChatResult result = chat_result(messages, opts);
  if (result.status == ChatStatus::Complete || !opts.json || opts.allow_partial)
    return std::move(result.content);
  return "";
}

ChatResult DeepSeekClient::chat_result(const std::vector<ChatMessage>& messages, const ChatOptions& opts) {
  if (!valid_options(opts)) {
    AME_LOGW("llm: invalid token limit or request timeout");
    return {};
  }
  const std::string key = cache_key(messages, opts);
  if (!cache_dir_.empty()) {
    std::ifstream input(cache_dir_ + "/" + key + ".txt", std::ios::binary);
    if (input) {
      std::stringstream contents;
      contents << input.rdbuf();
      if (!input.bad() && !contents.fail() && !contents.str().empty()) {
        ++cache_hits_;
        return {contents.str(), ChatStatus::Complete};
      }
    }
  }
  ChatResult result = call_api(messages, opts);
  if (result.status == ChatStatus::Complete && !result.content.empty() && !cache_dir_.empty()) {
    const std::string path = cache_dir_ + "/" + key + ".txt";
    CacheFile output(path);
    output.publish(path, result.content);
  }
  return result;
}

// chat_batch 伪代码：固定线程池（concurrency 个线程）+ 原子索引领取任务，
//   结果按下标写回，保持与请求同序。
std::vector<std::string> DeepSeekClient::chat_batch(const std::vector<ChatRequest>& reqs,
                                                    int concurrency) {
  std::vector<std::string> results(reqs.size());
  std::atomic<size_t> idx{0};
  if (concurrency < 1) concurrency = 1;
  int nthreads = std::min<int>(concurrency, (int)reqs.size());
  std::vector<std::thread> pool;
  for (int t = 0; t < nthreads; ++t) {
    pool.emplace_back([this, &idx, &reqs, &results] {
      for (;;) {
        size_t i = idx.fetch_add(1);
        if (i >= reqs.size()) break;
        results[i] = chat(reqs[i].messages, reqs[i].opts);
      }
    });
  }
  for (auto& th : pool) th.join();
  return results;
}

// from_config 伪代码：env AME_DEEPSEEK_API_KEY 优先；否则读 JSON 配置的 api_key；
//   两者皆无返回 nullptr；base_url 固定 DeepSeek，model/cache_dir 由调用方给。
std::unique_ptr<DeepSeekClient> DeepSeekClient::from_config(const std::string& config_path,
                                                            const std::string& model,
                                                            const std::string& cache_dir) {
  std::string key;
  const char* env = std::getenv("AME_DEEPSEEK_API_KEY");
  if (env && *env) {
    key = env;
  } else {
    std::ifstream f(config_path);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      bool ok = false;
      Json j = Json::parse(ss.str(), &ok);
      if (ok) {
        const Json* k = j.find("api_key");
        if (k) key = k->as_str();
      }
    }
  }
  if (key.empty()) return nullptr;
  return std::make_unique<DeepSeekClient>("https://api.deepseek.com", key, model, cache_dir);
}

// FakeLlmClient::chat 伪代码：记录请求全文；按注册顺序匹配 prompt 子串返回对应响应，
//   都不中返回 default_response。
std::string FakeLlmClient::chat(const std::vector<ChatMessage>& messages,
                                const ChatOptions& opts) {
  (void)opts;
  std::string all;
  for (auto& m : messages) all += m.content + "\n";
  received.push_back(all);
  for (auto& [needle, resp] : rules)
    if (all.find(needle) != std::string::npos) return resp;
  return default_response;
}

}  // namespace ame
