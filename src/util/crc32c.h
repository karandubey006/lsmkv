#pragma once

// CRC32C (Castagnoli polynomial, reflected 0x82F63B78), the checksum used by
// every on-disk record. Detects accidental corruption only: it is not a
// cryptographic hash.

#include <cstdint>
#include <string_view>

namespace lsmkv {

// Returns the CRC32C of `data` appended to bytes whose CRC32C is `crc`, so
//   Crc32c(b, Crc32c(a)) == Crc32c(a + b).
// The CRC32C of no bytes is 0.
uint32_t Crc32c(std::string_view data, uint32_t crc = 0);

// True if Crc32c uses the CPU's CRC32C instruction (ARMv8 CRC or SSE4.2).
bool Crc32cIsAccelerated();

// The table-driven implementation, exposed so tests can check the
// accelerated path against it.
uint32_t Crc32cPortable(std::string_view data, uint32_t crc = 0);

}  // namespace lsmkv
