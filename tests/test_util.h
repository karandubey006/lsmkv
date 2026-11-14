#pragma once

#include <gtest/gtest.h>
#include <stdlib.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

#define ASSERT_OK(expr)                                  \
  do {                                                   \
    ::lsmkv::Status _lsmkv_s = (expr);                   \
    ASSERT_TRUE(_lsmkv_s.ok()) << _lsmkv_s.ToString();   \
  } while (0)

#define EXPECT_OK(expr)                                  \
  do {                                                   \
    ::lsmkv::Status _lsmkv_s = (expr);                   \
    EXPECT_TRUE(_lsmkv_s.ok()) << _lsmkv_s.ToString();   \
  } while (0)

namespace lsmkv::test {

// A fresh, uniquely named directory, removed with its contents on destruction.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "lsmkv-XXXXXX").string();
    if (::mkdtemp(tmpl.data()) == nullptr) {
      ADD_FAILURE() << "mkdtemp failed for " << tmpl;
    }
    path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Renders arbitrary bytes readably for failure messages: printable ASCII as
// is, everything else as \xNN, long strings truncated.
inline std::string Printable(std::string_view s, size_t max_len = 48) {
  static const char* kHex = "0123456789abcdef";
  std::string out = "\"";
  for (size_t i = 0; i < s.size() && i < max_len; ++i) {
    auto c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c < 0x7f && c != '\\' && c != '"') {
      out.push_back(static_cast<char>(c));
    } else {
      out += "\\x";
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  out += "\"";
  if (s.size() > max_len) out += "...(" + std::to_string(s.size()) + " bytes)";
  return out;
}

}  // namespace lsmkv::test
