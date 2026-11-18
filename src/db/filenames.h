#pragma once

// Names of the files in a database directory.
//
//   LOCK              held for the lifetime of an open DB
//   CURRENT           "<manifest file name>\n": the MANIFEST in use
//   CURRENT.tmp       a CURRENT being written; renamed over CURRENT
//   NNNNNN.wal        write-ahead logs
//   NNNNNN.sst        sorted tables
//   MANIFEST-NNNNNN   logs of version edits
//
// WALs, SSTs and MANIFESTs share one number space, so a number names at most
// one file. Numbers are written in decimal with at least 6 digits.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lsmkv {

inline constexpr std::string_view kLockFileName = "LOCK";
inline constexpr std::string_view kCurrentFileName = "CURRENT";
inline constexpr std::string_view kCurrentTmpFileName = "CURRENT.tmp";

enum class FileKind {
  kLock,
  kCurrent,
  kCurrentTmp,
  kWal,
  kSst,
  kManifest,
};

struct ParsedFileName {
  FileKind kind;
  uint64_t number;  // 0 for kinds that have no number
};

std::string WalFileName(uint64_t number);
std::string SstFileName(uint64_t number);
std::string ManifestFileName(uint64_t number);

// The kind and number of a database file name, or nullopt for any other
// name. Only the canonical spelling of a number is accepted, so each
// (kind, number) has exactly one file name.
std::optional<ParsedFileName> ParseFileName(std::string_view name);

// The number of a WAL file name, or nullopt if name isn't one.
std::optional<uint64_t> ParseWalFileName(std::string_view name);

}  // namespace lsmkv
