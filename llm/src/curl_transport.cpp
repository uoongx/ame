#include "curl_transport.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ame::detail {
namespace {

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
  bool write(const std::string& text) const {
    if (fd_.get() < 0) return false;
    size_t offset = 0;
    while (offset < text.size()) {
      const auto count = ::write(fd_.get(), text.data() + offset, text.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += static_cast<size_t>(count);
    }
    return true;
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
  ~ChildProcess() {
    // Only our unreaped child reaches this path after capture fails (for example,
    // an oversized response). Terminate and reap it so cleanup cannot leave an
    // orphan process or wait indefinitely on a blocked stdout writer.
    if (pid_ > 0) { ::kill(pid_, SIGKILL); wait_for(pid_); }
  }
  int wait() { const int code = wait_for(pid_); pid_ = -1; return code; }
 private:
  pid_t pid_;
};

bool read_output(int descriptor, std::string& output) {
  constexpr size_t max_response = 64 * 1024 * 1024;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return false;
    if (count == 0) return true;
    if (static_cast<size_t>(count) > max_response - output.size()) return false;
    output.append(buffer, static_cast<size_t>(count));
  }
}

bool capture(const std::vector<std::string>& arguments, std::string& output) {
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
  SpawnActions actions;
  if (!actions.redirect(input.get(), writer.get())) return false;
  std::vector<char*> argv;
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (::posix_spawnp(&pid, argv.front(), actions.get(), nullptr, argv.data(), environ) != 0) return false;
  ChildProcess child(pid);
  writer.reset();
  if (!read_output(input.get(), output)) return false;
  return child.wait() == 0;
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
  if (!out || has_control(base_url) || has_control(api_key)) return 400;
  const TemporaryFile request("/tmp/ame_llm_body_XXXXXX");
  const TemporaryFile config("/tmp/ame_llm_conf_XXXXXX");
  const std::string headers = "header = \"Authorization: Bearer " + quote_config(api_key)
      + "\"\nheader = \"Content-Type: application/json\"\n";
  if (!request.write(body) || !config.write(headers)) return 0;
  // --disable must be first: user curlrc files must not change transport/security options.
  const std::vector<std::string> arguments = {
      "curl", "--disable", "-sS", "-m", "120", "--proto", "=http,https",
      "-K", config.path(), "-X", "POST", "--url", base_url + "/chat/completions",
      "--data-binary", std::string("@") + request.path(), "-w", "\\n%{http_code}"};
  std::string raw;
  if (!capture(arguments, raw)) return 0;
  return response_status(raw, out);
}

}  // namespace ame::detail
