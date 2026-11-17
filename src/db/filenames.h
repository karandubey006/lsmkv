#pragma once

// Names of the files in a database directory.
//
//   LOCK          held for the lifetime of an open DB
//   NNNNNN.wal    write-ahead logs; numbers increase, at least 6 digits

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lsmkv {

inline constexpr std::string_view kLockFileName = "LOCK";

std::string WalFileName(uint64_t number);

// The number of a WAL file name, or nullopt if name isn't one.
std::optional<uint64_t> ParseWalFileName(std::string_view name);

}  // namespace lsmkv
