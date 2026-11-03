#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ame_test {

// Linux tests own a unique 0700 directory inside their current working directory.
// mkdtemp creates it atomically; no predictable file is opened in a shared directory.
class TempDirectory {
 public:
  TempDirectory() {
    std::string pattern = (std::filesystem::current_path() / ".ame-test-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::system_error(errno, std::generic_category(), "Cannot create private test directory");
    directory_ = std::move(pattern);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  ~TempDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  const std::filesystem::path& root() const { return directory_; }

  std::string path(const std::string& leaf) const {
    if (leaf.empty() || leaf == "." || leaf == ".." ||
        leaf.find_first_of("/\\") != std::string::npos || leaf.find('\0') != std::string::npos)
      throw std::invalid_argument("Test path must be a single file name");
    return (directory_ / leaf).string();
  }

 private:
  std::filesystem::path directory_;
};

// Each test executable gets its own private namespace, including nonexistent paths.
inline std::string temp_path(const std::string& leaf) {
  static const TempDirectory directory;
  return directory.path(leaf);
}

inline void write_text_file(const std::string& path, const std::string& text,
                            std::ios::openmode mode = std::ios::trunc) {
  std::ofstream output(path, mode | std::ios::binary);
  if (!output) throw std::runtime_error("Cannot open test fixture: " + path);
  output << text;
  output.close();
  if (!output) throw std::runtime_error("Cannot write test fixture: " + path);
}

}  // namespace ame_test
