#include "util/crc32c.h"

#include <array>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define LSMKV_CRC32C_ARM 1
#elif defined(__SSE4_2__) && defined(__x86_64__)
#include <nmmintrin.h>
#define LSMKV_CRC32C_X86 1
#endif

namespace lsmkv {
namespace {

constexpr uint32_t kPolynomial = 0x82F63B78;  // reflected Castagnoli

// table[b] is the CRC register after shifting byte b through it bit by bit.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t b = 0; b < 256; ++b) {
    uint32_t r = b;
    for (int bit = 0; bit < 8; ++bit) r = (r & 1) ? (r >> 1) ^ kPolynomial : r >> 1;
    table[b] = r;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

// Both implementations work on the raw register; the public functions apply
// the standard pre- and post-inversion (~), which is what makes Crc32c of the
// empty string 0 and lets a CRC be extended across calls.
uint32_t UpdatePortable(uint32_t reg, const unsigned char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) reg = kTable[(reg ^ p[i]) & 0xff] ^ (reg >> 8);
  return reg;
}

#if defined(LSMKV_CRC32C_ARM) || defined(LSMKV_CRC32C_X86)
uint32_t UpdateAccelerated(uint32_t reg, const unsigned char* p, size_t n) {
  // 8 bytes per instruction; memcpy is the portable unaligned load.
  for (; n >= 8; p += 8, n -= 8) {
    uint64_t word;
    std::memcpy(&word, p, 8);
#if defined(LSMKV_CRC32C_ARM)
    reg = __crc32cd(reg, word);
#else
    reg = static_cast<uint32_t>(_mm_crc32_u64(reg, word));
#endif
  }
  for (; n > 0; ++p, --n) {
#if defined(LSMKV_CRC32C_ARM)
    reg = __crc32cb(reg, *p);
#else
    reg = _mm_crc32_u8(reg, *p);
#endif
  }
  return reg;
}
#endif

const unsigned char* Bytes(std::string_view data) {
  return reinterpret_cast<const unsigned char*>(data.data());
}

}  // namespace

uint32_t Crc32cPortable(std::string_view data, uint32_t crc) {
  return ~UpdatePortable(~crc, Bytes(data), data.size());
}

uint32_t Crc32c(std::string_view data, uint32_t crc) {
#if defined(LSMKV_CRC32C_ARM) || defined(LSMKV_CRC32C_X86)
  return ~UpdateAccelerated(~crc, Bytes(data), data.size());
#else
  return Crc32cPortable(data, crc);
#endif
}

bool Crc32cIsAccelerated() {
#if defined(LSMKV_CRC32C_ARM) || defined(LSMKV_CRC32C_X86)
  return true;
#else
  return false;
#endif
}

}  // namespace lsmkv
