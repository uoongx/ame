/*
 * 所属模块：core 公共基础层——极简日志接口与宏。
 * 本文件主体思路：提供全局日志级别设置（log_set_level）与带文件/行号的写日志函数
 *   （log_write），AME_LOGD/I/W/E 宏按参数顺序进行类型安全的流拼接。
 * 关键算法/数据结构：动态字符串缓冲，输出到 stderr（实现见 log.cpp）。
 * 用法：AME_LOGI("events=", count, ", text=", text)，文本不作为格式串解释。
 */
#pragma once
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace ame {
enum class LogLevel { Debug, Info, Warn, Error };
void log_set_level(LogLevel lv);
void log_write(LogLevel lv, const char* file, int line, const char* msg);

template <typename... Parts>
std::string log_message(Parts&&... parts) {
  std::ostringstream message;
  message.imbue(std::locale::classic());
  (message << ... << std::forward<Parts>(parts));
  return message.str();
}
}  // namespace ame

#define AME_LOG(lv, ...)                                                      \
  do {                                                                       \
    ::ame::log_write(lv, __FILE__, __LINE__,                                   \
                     ::ame::log_message(__VA_ARGS__).c_str());                \
  } while (0)

#define AME_LOGD(...) AME_LOG(::ame::LogLevel::Debug, __VA_ARGS__)
#define AME_LOGI(...) AME_LOG(::ame::LogLevel::Info, __VA_ARGS__)
#define AME_LOGW(...) AME_LOG(::ame::LogLevel::Warn, __VA_ARGS__)
#define AME_LOGE(...) AME_LOG(::ame::LogLevel::Error, __VA_ARGS__)
