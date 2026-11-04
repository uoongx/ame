#include "../llm/src/curl_transport.h"
#include "ame/core/json.h"
#include "test_util.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using ame::Json;
using Milliseconds = std::chrono::milliseconds;
constexpr Milliseconds kHangBudget{250};
constexpr Milliseconds kShortBudget{500};
constexpr Milliseconds kOversizedBudget = std::chrono::seconds{200};
constexpr auto kTotalTimeout = std::chrono::seconds{120};
constexpr auto kConnectTimeout = std::chrono::seconds{3};
constexpr auto kFakeWatchdog = std::chrono::seconds{3};
constexpr Milliseconds kStreamInterval{1};
constexpr Milliseconds kPollRoundingTolerance{10};
constexpr auto kTimingTolerance = std::chrono::seconds{1};

static std::string read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static void fake_timeout_behavior(const std::string& body) {
  if (body == "no-output" || body == "closed-output") {
    if (body == "closed-output") ::close(STDOUT_FILENO);
    ::alarm(static_cast<unsigned>(kFakeWatchdog.count()));
    for (;;) ::pause();
  }
  if (body == "stream-output") {
    ::alarm(static_cast<unsigned>(kFakeWatchdog.count()));
    const std::string chunk(1024, 'x');
    for (;;) {
      std::cout << chunk << std::flush;
      ::poll(nullptr, 0, static_cast<int>(kStreamInterval.count()));
    }
  }
}

// The test executable acts as curl, exercising the actual spawn/pipe/temporary-file path
// without a network request, a shell, a credential or an external curl dependency.
static int fake_curl(int argc, char** argv) {
  Json reply = Json::object();
  Json args = Json::array();
  for (int i = 1; i < argc; ++i) args.push(Json::string(argv[i]));
  reply["args"] = std::move(args);
  std::string body;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string option(argv[i]);
    if (option == "-K") {
      const std::string path(argv[i + 1]);
      reply["config"] = Json::string(read_file(path));
      reply["config_path"] = Json::string(path);
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600) return 2;
    }
    if (option == "--data-binary") {
      const std::string path = std::string(argv[i + 1]).substr(1);
      body = read_file(path);
      reply["body"] = Json::string(body);
      reply["body_path"] = Json::string(path);
      struct stat info{};
      if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 0777) != 0600) return 2;
    }
  }
  // The parent set PATH to the unique fixture directory. Record only filenames,
  // allowing the parent to check cleanup after unsuccessful curl exits too.
  const char* fixture_directory = std::getenv("PATH");
  if (!fixture_directory) return 3;
  {
    std::ofstream paths(fs::path(fixture_directory) / "captured-paths");
    paths << reply.find("config_path")->as_str() << '\n' << reply.find("body_path")->as_str() << '\n';
    std::ofstream process(fs::path(fixture_directory) / "captured-pid");
    process << ::getpid();
  }
  for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = fs::read_symlink(descriptor.path(), error).string();
    if (!error && target.find("/tmp/ame_llm_") == 0) return 4;
  }
  if (body == "exit-error") return 7;
  if (body == "bad-status") { std::cout << "ignored\n200junk"; return 0; }
  if (body == "large-status") { std::cout << "ignored\n99999999999999999999"; return 0; }
  fake_timeout_behavior(body);
  std::cout << reply.dump() << "\n200";
  return 0;
}

class TestEnvironment {
 public:
  TestEnvironment() {
    char pattern[] = "/tmp/ame_transport_test_XXXXXX";
    const char* created = ::mkdtemp(pattern);
    if (!created) throw std::runtime_error("mkdtemp failed");
    directory = created;
    const char* path = std::getenv("PATH");
    had_path = path != nullptr;
    old_path = path ? path : "";
    fs::create_symlink(fs::read_symlink("/proc/self/exe"), directory / "curl");
    if (::setenv("PATH", directory.c_str(), 1) != 0) throw std::runtime_error("setenv failed");
  }
  ~TestEnvironment() {
    if (had_path) ::setenv("PATH", old_path.c_str(), 1);
    else ::unsetenv("PATH");
    fs::remove_all(directory);
  }
  fs::path directory;
 private:
  bool had_path = false;
  std::string old_path;
};

static void test_transport() {
  TestEnvironment fixture;
  const std::string key = "test\\key\"quoted";
  const auto marker = fixture.directory / "must-not-exist";
  const std::string url = "https://example.invalid/;touch " + marker.string() + ";#";
  std::string response;
  CHECK(ame::detail::curl_transport(url, key, "{\"message\":\"hello\"}", &response) == 200);
  bool ok = false;
  Json reply = Json::parse(response, &ok);
  CHECK(ok);
  const auto* args = reply.find("args");
  CHECK(args && args->arr.size() == 18);
  if (!ok || !args || args->arr.size() != 18) return;
  CHECK(args->arr[0].as_str() == "--disable");
  CHECK(args->arr[2].as_str() == "-m");
  const double total_timeout = std::stod(args->arr[3].as_str());
  CHECK(total_timeout > 0.0 && total_timeout <= kTotalTimeout.count());
  CHECK(args->arr[4].as_str() == "--connect-timeout");
  CHECK(args->arr[5].as_str() == "3.000");
  CHECK(args->arr[12].as_str() == "--url");
  CHECK(args->arr[13].as_str() == url + "/chat/completions");
  CHECK(response.find("Authorization: Bearer test") != std::string::npos);
  CHECK(reply.find("config")->as_str().find("test\\\\key\\\"quoted") != std::string::npos);
  CHECK(reply.find("body")->as_str() == "{\"message\":\"hello\"}");
  CHECK(!fs::exists(marker));
  CHECK(!fs::exists(reply.find("config_path")->as_str()));
  CHECK(!fs::exists(reply.find("body_path")->as_str()));
  for (const auto* body : {"exit-error", "bad-status", "large-status"}) {
    response = "unchanged";
    CHECK(ame::detail::curl_transport("https://example.invalid", key, body, &response) == 0);
    CHECK(response == "unchanged");
    std::istringstream captured(read_file((fixture.directory / "captured-paths").string()));
    std::string path;
    while (std::getline(captured, path)) CHECK(!fs::exists(path));
  }
  CHECK(ame::detail::curl_transport(url, "key\noutput=\"bad\"", "body", &response) == 400);
  CHECK(ame::detail::curl_transport(std::string("https://x\0suffix", 16), key, "body", &response) == 400);
  CHECK(ame::detail::curl_transport(url, key, "body", nullptr) == 400);
}

static void check_child_cleanup(const TestEnvironment& fixture) {
  std::istringstream captured(read_file((fixture.directory / "captured-paths").string()));
  std::string path;
  size_t path_count = 0;
  while (std::getline(captured, path)) {
    CHECK(!fs::exists(path));
    ++path_count;
  }
  CHECK(path_count == 2);
  std::istringstream process(read_file((fixture.directory / "captured-pid").string()));
  pid_t pid = -1;
  process >> pid;
  CHECK(pid > 0);
  if (pid <= 0) return;
  int status = 0;
  errno = 0;
  CHECK(::waitpid(pid, &status, WNOHANG) == -1);
  CHECK(errno == ECHILD);
  CHECK(::kill(pid, 0) == -1);
  CHECK(errno == ESRCH);
}

static void test_deadline_cleanup() {
  TestEnvironment fixture;
  for (const auto* mode : {"no-output", "closed-output", "stream-output"}) {
    std::string response = "unchanged";
    const auto started = std::chrono::steady_clock::now();
    CHECK(ame::detail::curl_transport("https://example.invalid", "key", mode,
                                      &response, kHangBudget) == 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(response == "unchanged");
    CHECK(elapsed >= kHangBudget - kPollRoundingTolerance);
    CHECK(elapsed < kHangBudget + kTimingTolerance);
    check_child_cleanup(fixture);
  }
}

static void check_timeout_arguments(const std::string& response, Milliseconds budget) {
  bool ok = false;
  const auto reply = Json::parse(response, &ok);
  CHECK(ok);
  const auto* args = reply.find("args");
  CHECK(args && args->arr.size() == 18);
  if (!args || args->arr.size() != 18) return;
  CHECK(args->arr[2].as_str() == "-m");
  CHECK(args->arr[4].as_str() == "--connect-timeout");
  const double total = std::stod(args->arr[3].as_str());
  const double connect = std::stod(args->arr[5].as_str());
  const double seconds = std::chrono::duration<double>(budget).count();
  CHECK(total > 0.0 && total <= seconds && total <= kTotalTimeout.count());
  CHECK(connect > 0.0 && connect <= total && connect <= kConnectTimeout.count());
  if (budget < kConnectTimeout) CHECK(connect == total);
}

static void test_timeout_arguments() {
  TestEnvironment fixture;
  for (const auto budget : {kShortBudget, kOversizedBudget}) {
    std::string response;
    CHECK(ame::detail::curl_transport("https://example.invalid", "key", "success",
                                      &response, budget) == 200);
    check_timeout_arguments(response, budget);
    check_child_cleanup(fixture);
  }
}

static void test_expired_budget() {
  TestEnvironment fixture;
  for (const auto budget : {Milliseconds::zero(), Milliseconds{-1}, Milliseconds::min()}) {
    std::string response = "unchanged";
    CHECK(ame::detail::curl_transport("https://example.invalid", "key", "success",
                                      &response, budget) == 0);
    CHECK(response == "unchanged");
    CHECK(!fs::exists(fixture.directory / "captured-pid"));
    CHECK(!fs::exists(fixture.directory / "captured-paths"));
  }
}

int main(int argc, char** argv) {
  if (fs::path(argv[0]).filename() == "curl") return fake_curl(argc, argv);
  test_transport();
  test_deadline_cleanup();
  test_timeout_arguments();
  test_expired_budget();
  return AME_TEST_REPORT();
}
