#include "curl_transport.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ame::detail {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr Milliseconds kTotalTimeout = std::chrono::seconds{120};
constexpr Milliseconds kConnectTimeout = std::chrono::seconds{3};
constexpr Milliseconds kChildWaitInterval{10};

Milliseconds remaining_budget(Clock::time_point deadline) {
  return std::chrono::duration_cast<Milliseconds>(deadline - Clock::now());
}

std::string timeout_seconds(Milliseconds timeout) {
  constexpr auto milliseconds_per_second = Milliseconds(std::chrono::seconds{1}).count();
  const auto whole = timeout.count() / milliseconds_per_second;
  const auto fraction = timeout.count() % milliseconds_per_second;
  // Integer formatting is independent of the process locale.
  const std::string padded = std::to_string(milliseconds_per_second + fraction);
  return std::to_string(whole) + "." + padded.substr(1);
}

class FileDescriptor {
 public:
  explicit FileDescriptor(int value = -1) : value_(value) {}
  ~FileDescriptor() { reset(); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return value_; }
  void reset() { if (value_ >= 0) { ::close(value_); value_ = -1; } }
 private:
  int value_;
};

class TemporaryFile {
 public:
  explicit TemporaryFile(const char* pattern) : path_(pattern, pattern + std::char_traits<char>::length(pattern) + 1),
                                               fd_(::mkostemp(path_.data(), O_CLOEXEC)) {}
  ~TemporaryFile() { if (fd_.get() >= 0) ::unlink(path_.data()); }
  bool write(const std::string& text, Clock::time_point deadline) const {
    if (fd_.get() < 0) return false;
    size_t offset = 0;
    while (offset < text.size()) {
      if (Clock::now() >= deadline) return false;
      const auto count = ::write(fd_.get(), text.data() + offset, text.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += static_cast<size_t>(count);
    }
    return Clock::now() < deadline;
  }
  const char* path() const { return path_.data(); }
 private:
  std::vector<char> path_;
  FileDescriptor fd_;
};

class SpawnActions {
 public:
  SpawnActions() : valid_(posix_spawn_file_actions_init(&actions_) == 0) {}
  ~SpawnActions() { if (valid_) posix_spawn_file_actions_destroy(&actions_); }
  bool redirect(int input, int output) {
    return valid_ && posix_spawn_file_actions_adddup2(&actions_, output, STDOUT_FILENO) == 0
        && posix_spawn_file_actions_addclose(&actions_, input) == 0
        && posix_spawn_file_actions_addclose(&actions_, output) == 0;
  }
  const posix_spawn_file_actions_t* get() const { return &actions_; }
 private:
  posix_spawn_file_actions_t actions_{};
  bool valid_;
};

int wait_for(pid_t pid) {
  int status = 0;
  pid_t result;
  do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
  if (result != pid || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

class ChildProcess {
 public:
  explicit ChildProcess(pid_t pid) : pid_(pid) {}
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() {
    // Only our unreaped child reaches this path after capture fails (for example,
    // an oversized response). Terminate and reap it so cleanup cannot leave an
    // orphan process or wait indefinitely on a blocked stdout writer.
    if (pid_ > 0) { ::kill(pid_, SIGKILL); wait_for(pid_); }
  }
  bool wait_until(Clock::time_point deadline) {
    while (Clock::now() < deadline) {
      const auto state = poll_exit();
      if (state != ExitState::pending) return state == ExitState::success;
      const auto delay = std::min(remaining_budget(deadline), kChildWaitInterval);
      if (delay.count() <= 0) return false;
      if (::poll(nullptr, 0, static_cast<int>(delay.count())) < 0 && errno != EINTR) return false;
    }
    return false;
  }
 private:
  enum class ExitState { pending, success, failure };
  ExitState poll_exit() {
    int status = 0;
    const auto result = ::waitpid(pid_, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return ExitState::pending;
    if (result == pid_) {
      pid_ = -1;
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? ExitState::success : ExitState::failure;
    }
    // An externally reaped child is no longer ours to signal.
    if (errno == ECHILD) pid_ = -1;
    return ExitState::failure;
  }
  pid_t pid_;
};

bool wait_readable(int descriptor, Clock::time_point deadline) {
  for (;;) {
    const auto remaining = remaining_budget(deadline);
    if (remaining.count() <= 0) return false;
    pollfd event{descriptor, POLLIN, 0};
    const int result = ::poll(&event, 1, static_cast<int>(remaining.count()));
    if (result > 0) {
      return (static_cast<unsigned>(event.revents) & static_cast<unsigned>(POLLNVAL)) == 0;
    }
    if (result == 0 || errno != EINTR) return false;
  }
}

bool read_output(int descriptor, std::string& output, Clock::time_point deadline) {
  constexpr size_t max_response = 64 * 1024 * 1024;
  char buffer[4096];
  while (Clock::now() < deadline) {
    const auto count = ::read(descriptor, buffer, sizeof(buffer));
    if (count > 0) {
      if (static_cast<size_t>(count) > max_response - output.size()) return false;
      output.append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count == 0) return true;
    if (count < 0 && errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    if (!wait_readable(descriptor, deadline)) return false;
  }
  return false;
}

bool make_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0) return false;
  const auto updated = static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK);
  return ::fcntl(descriptor, F_SETFL, static_cast<int>(updated)) == 0;
}

bool capture(const std::vector<std::string>& arguments, std::string& output,
             Clock::time_point deadline) {
  int descriptors[2];
  if (::pipe2(descriptors, O_CLOEXEC) != 0) return false;
  FileDescriptor pipe_input(descriptors[0]);
  FileDescriptor pipe_writer(descriptors[1]);
  // Reserve descriptors above stdio even if the caller has closed stdin/stdout.
  const FileDescriptor input(::fcntl(pipe_input.get(), F_DUPFD_CLOEXEC, 3));
  FileDescriptor writer(::fcntl(pipe_writer.get(), F_DUPFD_CLOEXEC, 3));
  pipe_input.reset();
  pipe_writer.reset();
  if (input.get() < 0 || writer.get() < 0) return false;
  if (!make_nonblocking(input.get())) return false;
  SpawnActions actions;
  if (!actions.redirect(input.get(), writer.get())) return false;
  std::vector<char*> argv;
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  if (Clock::now() >= deadline) return false;
  pid_t pid = -1;
  if (::posix_spawnp(&pid, argv.front(), actions.get(), nullptr, argv.data(), environ) != 0) return false;
  ChildProcess child(pid);
  writer.reset();
  if (!read_output(input.get(), output, deadline)) return false;
  return child.wait_until(deadline);
}

bool has_control(const std::string& text) {
  for (const unsigned char c : text) if (c < 0x20 || c == 0x7f) return true;
  return false;
}

std::string quote_config(const std::string& text) {
  std::string result;
  for (char c : text) {
    if (c == '"' || c == '\\') result += '\\';
    result += c;
  }
  return result;
}

int response_status(const std::string& raw, std::string* out) {
  const auto newline = raw.rfind('\n');
  if (newline == std::string::npos || raw.size() - newline != 4) return 0;
  int code = 0;
  const auto parsed = std::from_chars(raw.data() + newline + 1, raw.data() + raw.size(), code);
  if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) return 0;
  if (code < 100 || code > 599) return 0;
  *out = raw.substr(0, newline);
  return code;
}

}  // namespace

int curl_transport(const std::string& base_url, const std::string& api_key,
                   const std::string& body, std::string* out) {
  return curl_transport(base_url, api_key, body, out, kTotalTimeout);
}

int curl_transport(const std::string& base_url, const std::string& api_key,
                   const std::string& body, std::string* out, Milliseconds timeout) {
  const auto started = Clock::now();
  if (timeout.count() <= 0) return 0;
  const auto deadline = started + std::min(timeout, kTotalTimeout);
  if (!out || has_control(base_url) || has_control(api_key)) return 400;
  const TemporaryFile request("/tmp/ame_llm_body_XXXXXX");
  const TemporaryFile config("/tmp/ame_llm_conf_XXXXXX");
  const std::string headers = "header = \"Authorization: Bearer " + quote_config(api_key)
      + "\"\nheader = \"Content-Type: application/json\"\n";
  if (!request.write(body, deadline) || !config.write(headers, deadline)) return 0;
  const auto remaining = remaining_budget(deadline);
  if (remaining.count() <= 0) return 0;
  // --disable must be first: user curlrc files must not change transport/security options.
  const std::vector<std::string> arguments = {
      "curl", "--disable", "-sS", "-m", timeout_seconds(remaining),
      "--connect-timeout", timeout_seconds(std::min(remaining, kConnectTimeout)), "--proto", "=http,https",
      "-K", config.path(), "-X", "POST", "--url", base_url + "/chat/completions",
      "--data-binary", std::string("@") + request.path(), "-w", "\\n%{http_code}"};
  std::string raw;
  if (!capture(arguments, raw, deadline)) return 0;
  return response_status(raw, out);
}

}  // namespace ame::detail
