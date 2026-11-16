#include "db/dbformat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "util/coding.h"

namespace lsmkv {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string result;
  AppendInternalKey(&result, user_key, seq, type);
  return result;
}

TEST(DBFormatTest, TrailerLayout) {
  const std::string k = IKey("ab", 0x0102, ValueType::kValue);
  ASSERT_EQ(k.size(), 2 + kTrailerSize);
  EXPECT_EQ(k.substr(0, 2), "ab");
  EXPECT_EQ(DecodeFixed64(k.data() + 2), (uint64_t{0x0102} << 8) | 1);
  EXPECT_EQ(static_cast<uint8_t>(k[2]), 0x01);  // the type is the lowest byte
  EXPECT_EQ(TrailerOf(k), (uint64_t{0x0102} << 8) | 1);
  EXPECT_EQ(TypeOf(k), ValueType::kValue);
  EXPECT_EQ(UserKeyOf(k), "ab");
}

TEST(DBFormatTest, DecodeRoundTrip) {
  const std::vector<std::string> user_keys = {"", "a", std::string("\0\xff", 2), "user000042"};
  const std::vector<SequenceNumber> seqs = {0, 1, 255, 256, kMaxSequenceNumber};
  for (const auto& uk : user_keys) {
    for (SequenceNumber seq : seqs) {
      for (ValueType t : {ValueType::kValue, ValueType::kDeletion}) {
        const std::string ikey = IKey(uk, seq, t);  // the decoded user_key views it
        const auto parts = DecodeInternalKey(ikey);
        ASSERT_TRUE(parts.has_value());
        EXPECT_EQ(parts->user_key, uk);
        EXPECT_EQ(parts->sequence, seq);
        EXPECT_EQ(parts->type, t);
      }
    }
  }
}

TEST(DBFormatTest, DecodeRejectsBadKeys) {
  EXPECT_FALSE(DecodeInternalKey("short").has_value());
  std::string bad_type = "k";
  PutFixed64(&bad_type, (uint64_t{5} << 8) | 7);  // type 7 doesn't exist
  EXPECT_FALSE(DecodeInternalKey(bad_type).has_value());
}

TEST(DBFormatTest, ComparatorOrder) {
  auto less = [](const std::string& a, const std::string& b) {
    return CompareInternalKeys(a, b) < 0;
  };

  // User key ascending, byte-wise unsigned.
  EXPECT_TRUE(less(IKey("a", 1, ValueType::kValue), IKey("b", 1, ValueType::kValue)));
  EXPECT_TRUE(less(IKey("a", 1, ValueType::kValue), IKey("ab", 9, ValueType::kValue)));
  EXPECT_TRUE(less(IKey("z", 1, ValueType::kValue), IKey("\xff", 1, ValueType::kValue)));
  EXPECT_TRUE(less(IKey("", 1, ValueType::kValue), IKey("a", 1, ValueType::kValue)));

  // User keys are compared on their own: as raw bytes, "a"+trailer (0x61 0x01 ...)
  // would sort after "a\0"+trailer (0x61 0x00 ...), but "a" < "a\0".
  EXPECT_TRUE(
      less(IKey("a", 1, ValueType::kValue), IKey(std::string("a\0", 2), 1, ValueType::kValue)));

  // Same user key: sequence descending, then type descending.
  EXPECT_TRUE(less(IKey("a", 9, ValueType::kValue), IKey("a", 1, ValueType::kValue)));
  EXPECT_TRUE(less(IKey("a", 5, ValueType::kValue), IKey("a", 5, ValueType::kDeletion)));

  EXPECT_EQ(CompareInternalKeys(IKey("a", 5, ValueType::kValue), IKey("a", 5, ValueType::kValue)),
            0);
}

TEST(DBFormatTest, NewestVersionSortsFirst) {
  std::vector<std::string> keys = {
      IKey("apple", 2, ValueType::kValue),
      IKey("apple", 5, ValueType::kDeletion),
      IKey("banana", 6, ValueType::kValue),
      IKey("apple", 7, ValueType::kValue),
  };
  std::sort(keys.begin(), keys.end(),
            [](const std::string& a, const std::string& b) { return CompareInternalKeys(a, b) < 0; });
  EXPECT_EQ(keys[0], IKey("apple", 7, ValueType::kValue));
  EXPECT_EQ(keys[1], IKey("apple", 5, ValueType::kDeletion));
  EXPECT_EQ(keys[2], IKey("apple", 2, ValueType::kValue));
  EXPECT_EQ(keys[3], IKey("banana", 6, ValueType::kValue));
}

TEST(DBFormatTest, SeekKeyBoundsTheVisibleVersions) {
  const std::string seek = SeekKey("apple", 5);
  // Sorts at or before every apple entry with sequence <= 5 ...
  EXPECT_LT(CompareInternalKeys(seek, IKey("apple", 5, ValueType::kDeletion)), 0);
  EXPECT_EQ(CompareInternalKeys(seek, IKey("apple", 5, ValueType::kValue)), 0);
  EXPECT_LT(CompareInternalKeys(seek, IKey("apple", 4, ValueType::kValue)), 0);
  // ... and after every newer one.
  EXPECT_GT(CompareInternalKeys(seek, IKey("apple", 6, ValueType::kDeletion)), 0);
  EXPECT_EQ(UserKeyOf(seek), "apple");
}

}  // namespace
}  // namespace lsmkv
