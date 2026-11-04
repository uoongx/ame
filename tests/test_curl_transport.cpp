#include "../llm/src/curl_transport.h"
#include "ame/core/json.h"
#include "test_util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using ame::Json;

static std::string read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
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
  }
  for (const auto& descriptor : fs::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = fs::read_symlink(descriptor.path(), error).string();
    if (!error && target.find("/tmp/ame_llm_") == 0) return 4;
  }
  if (body == "exit-error") return 7;
  if (body == "bad-status") { std::cout << "ignored\n200junk"; return 0; }
  if (body == "large-status") { std::cout << "ignored\n99999999999999999999"; return 0; }
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
  CHECK(args->arr[3].as_str() == "120");
  CHECK(args->arr[4].as_str() == "--connect-timeout");
  CHECK(args->arr[5].as_str() == "3");
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

int main(int argc, char** argv) {
  if (fs::path(argv[0]).filename() == "curl") return fake_curl(argc, argv);
  test_transport();
  return AME_TEST_REPORT();
}
