// sst_dump: prints the footer, index and every entry of an SST file.
//
//   sst_dump <path-to-.sst>
//
// Exits non-zero if the file can't be read or is corrupt.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "env/env.h"
#include "table/format.h"
#include "table/sst_reader.h"

namespace {

using lsmkv::Status;

// Printable ASCII as is, other bytes as \xNN, cut off after max_len bytes.
std::string Escape(std::string_view s, size_t max_len = 64) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < s.size() && i < max_len; ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else {
      out += "\\x";
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  if (s.size() > max_len) out += "...(" + std::to_string(s.size()) + " bytes)";
  return out;
}

std::string FormatKey(std::string_view internal_key) {
  const auto parts = lsmkv::DecodeInternalKey(internal_key);
  if (!parts) return "<malformed internal key '" + Escape(internal_key) + "'>";
  return "'" + Escape(parts->user_key) + "' @" + std::to_string(parts->sequence) + " " +
         (parts->type == lsmkv::ValueType::kValue ? "PUT" : "DEL");
}

int Fail(const std::string& what, const Status& s) {
  std::fprintf(stderr, "sst_dump: %s: %s\n", what.c_str(), s.ToString().c_str());
  return 1;
}

int Dump(const std::string& path) {
  const std::filesystem::path fs_path(path);
  std::string dir_path = fs_path.parent_path().string();
  if (dir_path.empty()) dir_path = ".";
  const std::string name = fs_path.filename().string();

  std::unique_ptr<lsmkv::Directory> dir;
  Status s = lsmkv::Env::Posix()->OpenDirectory(dir_path, &dir);
  if (!s.ok()) return Fail(dir_path, s);
  std::unique_ptr<lsmkv::ReadableFile> file;
  s = dir->Open(name, &file);
  if (!s.ok()) return Fail(path, s);
  uint64_t file_size = 0;
  s = file->Size(&file_size);
  if (!s.ok()) return Fail(path, s);

  std::unique_ptr<lsmkv::SstReader> table;
  s = lsmkv::SstReader::Open(std::move(file), file_size, &table);
  if (!s.ok()) return Fail(path, s);

  const lsmkv::Footer& footer = table->footer();
  std::printf("footer:\n");
  std::printf("  format_version  %u\n", footer.format_version);
  std::printf("  index           offset %llu size %llu\n",
              static_cast<unsigned long long>(footer.index.offset),
              static_cast<unsigned long long>(footer.index.size));
  std::printf("  filter          offset %llu size %llu\n",
              static_cast<unsigned long long>(footer.filter.offset),
              static_cast<unsigned long long>(footer.filter.size));

  std::printf("index:\n");
  uint64_t num_blocks = 0;
  uint64_t num_entries = 0;
  auto index_iter = table->NewIndexIterator();
  for (index_iter->SeekToFirst(); index_iter->Valid(); index_iter->Next()) {
    lsmkv::BlockHandle handle;
    std::string_view encoded = index_iter->value();
    s = handle.DecodeFrom(&encoded);
    if (!s.ok()) return Fail("index entry " + std::to_string(num_blocks), s);
    std::printf("  block %llu: offset %llu size %llu last %s\n",
                static_cast<unsigned long long>(num_blocks),
                static_cast<unsigned long long>(handle.offset),
                static_cast<unsigned long long>(handle.size),
                FormatKey(index_iter->key()).c_str());
    ++num_blocks;
  }
  if (s = index_iter->status(); !s.ok()) return Fail("index block", s);

  std::printf("entries:\n");
  auto iter = table->NewIterator();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    std::printf("  %s => '%s'\n", FormatKey(iter->key()).c_str(), Escape(iter->value()).c_str());
    ++num_entries;
  }
  if (s = iter->status(); !s.ok()) {
    return Fail("data block after entry " + std::to_string(num_entries), s);
  }

  std::printf("summary: %llu entries, %llu data blocks, %llu bytes\n",
              static_cast<unsigned long long>(num_entries),
              static_cast<unsigned long long>(num_blocks),
              static_cast<unsigned long long>(file_size));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: sst_dump <path-to-.sst>\n");
    return 2;
  }
  return Dump(argv[1]);
}
