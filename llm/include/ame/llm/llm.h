/*
 * 所属模块：llm 接入层（无 M 编号，工程附加层）——DeepSeek LLM 客户端统一接口。
 * 本文件主体思路：定义 ILlmClient 抽象接口（chat 单轮对话）与 DeepSeekClient 实现；
 *   HTTP 传输走 curl 子进程（零第三方依赖约束下的折中，Transport 可注入替换——
 *   生产可换 libcurl，测试注入 fake transport，单元测试不打真实 API）；
 *   磁盘缓存（请求哈希 → 响应）保证断点续跑不重复扣费。
 * 关键算法/数据结构：缓存键 = FNV-1a(模型+消息+选项) 的 64 位哈希文件名；
 *   Retries: transient failures and token expansion share a total time budget.
 * 安全约束：API key 从 bench/config/deepseek.local.json 读取，
 *   环境变量 AME_DEEPSEEK_API_KEY 优先；key 不出现在任何日志/输出中。
 * 依赖关系：依赖 core(json.h/types.h)；被 llm 组件层（components.h）与 bench 依赖。
 */
#pragma once
#include "ame/core/types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ame {

struct ChatMessage {
  std::string role;     // system|user|assistant
  std::string content;
};

inline constexpr auto kDefaultLlmTimeout = std::chrono::seconds{120};

struct ChatOptions {
  double temperature = 0.0;
  int max_tokens = 1024;
  bool json = false;                  // response_format=json_object
  bool disable_thinking = true;       // deepseek-v4 推理模型：默认关思考
                                      //（thinking:{type:disabled}），避免 reasoning 烧光预算
  // Total network retry budget, including backoff; cache I/O is outside this budget.
  std::chrono::milliseconds timeout{kDefaultLlmTimeout};
  // Plain text may be partial; JSON requires explicit opt-in to partial content.
  bool allow_partial = false;
};

enum class ChatStatus { Complete, Truncated, Failed, DeadlineExceeded };

struct ChatResult {
  // Content can retain a partial response even when a later attempt fails or times out.
  std::string content;
  ChatStatus status = ChatStatus::Failed;
};

// Configure before concurrent calls. Injected callbacks must be thread-safe.
struct LlmRetryRuntime {
  using Clock = std::chrono::steady_clock;
  std::function<Clock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<std::chrono::milliseconds(int)> delay;
};

struct ChatRequest {
  std::vector<ChatMessage> messages;
  ChatOptions opts;
};

class ILlmClient {
 public:
  virtual ~ILlmClient() = default;
  // Return text according to the implementation's documented partial-response policy.
  virtual std::string chat(const std::vector<ChatMessage>& messages,
                           const ChatOptions& opts) = 0;
  // 批量：默认顺序逐条（子类可覆盖为并发实现）
  virtual std::vector<std::string> chat_batch(const std::vector<ChatRequest>& reqs,
                                              int concurrency = 8) {
    (void)concurrency;
    std::vector<std::string> out;
    for (auto& r : reqs) out.push_back(chat(r.messages, r.opts));
    return out;
  }
};

// 传输层抽象：输入请求体 JSON，输出 HTTP 状态码（0=传输失败）与响应体
using LlmTransport =
    std::function<int(const std::string& body, std::string* out)>;
// Implementations must bound their I/O by the supplied remaining time.
using LlmTimedTransport = std::function<int(const std::string& body, std::string* out,
                                          std::chrono::milliseconds remaining)>;

class DeepSeekClient : public ILlmClient {
 public:
  // base_url 形如 https://api.deepseek.com（端点 /chat/completions 在实现内拼接）
  DeepSeekClient(std::string base_url, std::string api_key, std::string model,
                 std::string cache_dir);
  // The default transport captures this instance; keep its address stable.
  DeepSeekClient(const DeepSeekClient&) = delete;
  DeepSeekClient& operator=(const DeepSeekClient&) = delete;
  DeepSeekClient(DeepSeekClient&&) = delete;
  DeepSeekClient& operator=(DeepSeekClient&&) = delete;

  std::string chat(const std::vector<ChatMessage>& messages,
                   const ChatOptions& opts) override;
  // Exposes partial content and completion status; only complete responses are cached.
  ChatResult chat_result(const std::vector<ChatMessage>& messages, const ChatOptions& opts);

  // 批量并发：固定线程池 + 原子索引分派，结果与请求同序
  std::vector<std::string> chat_batch(const std::vector<ChatRequest>& reqs,
                                      int concurrency = 8) override;

  // Configure transports and runtime before starting concurrent calls.
  // Legacy adapters must enforce their own I/O limits; prefer set_timed_transport.
  void set_transport(LlmTransport transport);
  void set_timed_transport(LlmTimedTransport transport);
  void set_retry_runtime(LlmRetryRuntime runtime);

  // 统计（测试/成本核算用）
  int api_calls() const { return api_calls_; }
  int cache_hits() const { return cache_hits_; }

  // 从配置文件构造：env AME_DEEPSEEK_API_KEY 优先，其次 JSON 配置 {"api_key":...}
  // 配置缺失返回 nullptr
  static std::unique_ptr<DeepSeekClient> from_config(const std::string& config_path,
                                                     const std::string& model,
                                                     const std::string& cache_dir);

 private:
  std::string cache_key(const std::vector<ChatMessage>& messages,
                        const ChatOptions& opts) const;
  ChatResult call_api(const std::vector<ChatMessage>& messages, ChatOptions opts);
  std::string build_body(const std::vector<ChatMessage>& messages,
                         const ChatOptions& opts) const;

  std::string base_url_, api_key_, model_, cache_dir_;
  LlmTimedTransport transport_;
  LlmRetryRuntime runtime_;
  std::atomic<int> api_calls_{0};
  std::atomic<int> cache_hits_{0};
};

// 测试用假客户端：按注册顺序或规则返回固定响应，记录收到的请求
class FakeLlmClient : public ILlmClient {
 public:
  std::string default_response;
  std::vector<std::pair<std::string, std::string>> rules;  // prompt 子串 -> 响应
  std::vector<std::string> received;

  std::string chat(const std::vector<ChatMessage>& messages,
                   const ChatOptions& opts) override;
};

}  // namespace ame
