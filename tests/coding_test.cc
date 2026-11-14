#include "util/coding.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace lsmkv {
namespace {

TEST(CodingTest, Fixed32IsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201);
  ASSERT_EQ(s.size(), 4u);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04", 4));
  EXPECT_EQ(DecodeFixed32(s.data()), 0x04030201u);
}

TEST(CodingTest, Fixed64RoundTrip) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{0xFF}, uint64_t{0x0102030405060708},
                     std::numeric_limits<uint64_t>::max()}) {
    std::string s;
    PutFixed64(&s, v);
    ASSERT_EQ(s.size(), 8u);
    EXPECT_EQ(DecodeFixed64(s.data()), v);
  }
}

TEST(CodingTest, VarintKnownEncoding) {
  std::string s;
  PutVarint32(&s, 300);
  EXPECT_EQ(s, std::string("\xAC\x02", 2));
}

TEST(CodingTest, Varint64RoundTripBoundaries) {
  std::vector<uint64_t> values;
  for (int shift = 0; shift < 64; ++shift) {
    uint64_t p = uint64_t{1} << shift;
    values.push_back(p - 1);
    values.push_back(p);
    values.push_back(p + 1);
  }
  values.push_back(std::numeric_limits<uint64_t>::max());

  std::string s;
  for (uint64_t v : values) PutVarint64(&s, v);

  std::string_view input(s);
  for (uint64_t expected : values) {
    uint64_t actual = 0;
    size_t before = input.size();
    ASSERT_TRUE(GetVarint64(&input, &actual));
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(before - input.size(), static_cast<size_t>(VarintLength(expected)));
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, TruncatedVarintFailsAndLeavesInputUnchanged) {
  std::string s;
  PutVarint64(&s, std::numeric_limits<uint64_t>::max());
  for (size_t len = 0; len < s.size(); ++len) {
    std::string_view input(s.data(), len);
    uint64_t v = 0;
    EXPECT_FALSE(GetVarint64(&input, &v)) << "len=" << len;
    EXPECT_EQ(input.size(), len);
  }
}

TEST(CodingTest, Varint32RejectsValuesAbove32Bits) {
  std::string s;
  PutVarint64(&s, uint64_t{1} << 32);
  std::string_view input(s);
  uint32_t v = 0;
  EXPECT_FALSE(GetVarint32(&input, &v));
  EXPECT_EQ(input.size(), s.size());
}

TEST(CodingTest, Varint64RejectsOverflow) {
  // Eleven continuation bytes: longer than any valid uint64 varint.
  std::string s(10, '\xFF');
  s.push_back('\x01');
  std::string_view input(s);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&input, &v));
}

TEST(CodingTest, LengthPrefixedRoundTripWithEmbeddedNul) {
  std::string s;
  PutLengthPrefixed(&s, "");
  PutLengthPrefixed(&s, std::string_view("a\0b", 3));
  PutLengthPrefixed(&s, std::string(1000, 'x'));

  std::string_view input(s), out;
  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, "");
  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, std::string_view("a\0b", 3));
  ASSERT_TRUE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(out, std::string(1000, 'x'));
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, LengthPrefixedRejectsShortBody) {
  std::string s;
  PutLengthPrefixed(&s, "hello");
  s.pop_back();
  std::string_view input(s), out;
  EXPECT_FALSE(GetLengthPrefixed(&input, &out));
  EXPECT_EQ(input.size(), s.size());
}

}  // namespace
}  // namespace lsmkv
