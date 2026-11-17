#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace lsmkv {
namespace {

std::string Bytes(size_t n, auto byte_at) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(byte_at(i));
  return s;
}

// Test vectors from RFC 3720 (iSCSI), appendix B.4, plus the usual check value.
TEST(Crc32cTest, KnownValues) {
  for (auto crc : {Crc32c, Crc32cPortable}) {
    EXPECT_EQ(crc(Bytes(32, [](size_t) { return 0x00; }), 0), 0x8A9136AAu);
    EXPECT_EQ(crc(Bytes(32, [](size_t) { return 0xFF; }), 0), 0x62A8AB43u);
    EXPECT_EQ(crc(Bytes(32, [](size_t i) { return i; }), 0), 0x46DD794Eu);
    EXPECT_EQ(crc(Bytes(32, [](size_t i) { return 31 - i; }), 0), 0x113FDB5Cu);
    EXPECT_EQ(crc("123456789", 0), 0xE3069283u);
  }
}

TEST(Crc32cTest, EmptyInputIsZeroButZeroBytesAreNot) {
  // Why the WAL's CRC covers the length field: a zeroed header with a zero
  // length would otherwise carry a "valid" CRC of 0.
  EXPECT_EQ(Crc32c(""), 0u);
  EXPECT_NE(Crc32c(std::string(4, '\0')), 0u);
}

TEST(Crc32cTest, ExtendingEqualsWhole) {
  const std::string data = "the quick brown fox jumps over the lazy dog";
  for (size_t split = 0; split <= data.size(); ++split) {
    const std::string_view a = std::string_view(data).substr(0, split);
    const std::string_view b = std::string_view(data).substr(split);
    EXPECT_EQ(Crc32c(b, Crc32c(a)), Crc32c(data)) << split;
  }
}

TEST(Crc32cTest, AcceleratedMatchesPortable) {
  std::mt19937 rng(42);
  std::string data(10000, '\0');
  for (char& c : data) c = static_cast<char>(rng());
  // Every length and misalignment around the 8-byte fast path.
  for (size_t offset = 0; offset < 9; ++offset) {
    for (size_t len = 0; len < 100; ++len) {
      std::string_view s = std::string_view(data).substr(offset, len);
      ASSERT_EQ(Crc32c(s, 123), Crc32cPortable(s, 123)) << offset << " " << len;
    }
  }
  EXPECT_EQ(Crc32c(data), Crc32cPortable(data));
}

TEST(Crc32cTest, DetectsEverySingleBitFlip) {
  std::string data = "some WAL payload bytes";
  const uint32_t good = Crc32c(data);
  for (size_t i = 0; i < data.size() * 8; ++i) {
    data[i / 8] ^= static_cast<char>(1 << (i % 8));
    EXPECT_NE(Crc32c(data), good) << "bit " << i;
    data[i / 8] ^= static_cast<char>(1 << (i % 8));
  }
}

}  // namespace
}  // namespace lsmkv
