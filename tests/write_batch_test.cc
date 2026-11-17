#include "lsmkv/write_batch.h"

#include <gtest/gtest.h>

#include <random>

#include "db/batch_format.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

// Renders an encoded batch as "Put(k,v)Delete(k)..." to make order easy to
// assert, and reports how the cursor stopped.
std::string Print(std::string_view encoded, Status* status = nullptr) {
  BatchCursor cursor(encoded);
  BatchOp op;
  std::string out;
  while (cursor.Next(&op)) {
    if (op.type == ValueType::kValue) {
      out += "Put(" + std::string(op.key) + "," + std::string(op.value) + ")";
    } else {
      out += "Delete(" + std::string(op.key) + ")";
    }
  }
  if (status != nullptr) *status = cursor.status();
  return out;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(b.Encoded().size(), kBatchHeaderSize);
  Status s;
  EXPECT_EQ(Print(b.Encoded(), &s), "");
  EXPECT_OK(s);
}

TEST(WriteBatchTest, PreservesOrder) {
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  b.Put("a", "2");
  EXPECT_EQ(b.Count(), 3u);
  EXPECT_EQ(Print(b.Encoded()), "Put(a,1)Delete(b)Put(a,2)");
  EXPECT_OK(ValidateBatch(b.Encoded()));
}

TEST(WriteBatchTest, Clear) {
  WriteBatch b;
  b.Put("a", "1");
  b.Clear();
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(Print(b.Encoded()), "");
  EXPECT_EQ(b.Encoded().size(), kBatchHeaderSize);
}

TEST(WriteBatchTest, BinaryKeysAndEmptyValues) {
  WriteBatch b;
  b.Put(std::string_view("k\0x", 3), "");
  b.Put("", "v");
  BatchCursor cursor(b.Encoded());
  BatchOp op;
  ASSERT_TRUE(cursor.Next(&op));
  EXPECT_EQ(op.key, std::string_view("k\0x", 3));
  EXPECT_EQ(op.value, "");
  ASSERT_TRUE(cursor.Next(&op));
  EXPECT_EQ(op.key, "");
  EXPECT_EQ(op.value, "v");
  EXPECT_FALSE(cursor.Next(&op));
  EXPECT_OK(cursor.status());
}

TEST(WriteBatchTest, OpTypeByteIsTheValueType) {
  WriteBatch b;
  b.Delete("k");
  EXPECT_EQ(b.Encoded()[kBatchHeaderSize], static_cast<char>(ValueType::kDeletion));
}

TEST(WriteBatchTest, EveryTruncationIsCorruption) {
  WriteBatch b;
  b.Put("key", "value");
  b.Delete("gone");
  b.Put("", "");
  const std::string full(b.Encoded());
  for (size_t cut = 0; cut < full.size(); ++cut) {
    EXPECT_TRUE(ValidateBatch(full.substr(0, cut)).IsCorruption()) << "cut " << cut;
  }
  EXPECT_OK(ValidateBatch(full));
}

TEST(WriteBatchTest, CountMismatchIsCorruption) {
  WriteBatch b;
  b.Put("k", "v");
  std::string bytes(b.Encoded());
  EncodeFixed32(bytes.data(), 2);  // header claims 2 ops, body has 1
  Status s;
  EXPECT_EQ(Print(bytes, &s), "Put(k,v)");  // the cursor yields ops before it finds out
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_TRUE(ValidateBatch(bytes).IsCorruption());
}

TEST(WriteBatchTest, UnknownOpTypeIsCorruption) {
  WriteBatch b;
  b.Put("k", "v");
  std::string bytes(b.Encoded());
  bytes[kBatchHeaderSize] = 0x7F;
  EXPECT_TRUE(ValidateBatch(bytes).IsCorruption());
}

TEST(WriteBatchTest, RandomBytesNeverCrashTheCursor) {
  std::mt19937 rng(5);
  for (int i = 0; i < 5000; ++i) {
    std::string bytes(rng() % 40, '\0');
    for (char& c : bytes) c = static_cast<char>(rng() % 4 == 0 ? rng() : rng() % 3);
    Status s = ValidateBatch(bytes);
    EXPECT_TRUE(s.ok() || s.IsCorruption());
  }
}

TEST(WalPayloadTest, RoundTrip) {
  WriteBatch b;
  b.Put("k", "v");
  const std::string payload = EncodeWalPayloadHeader(0x0102030405060708ull) + std::string(b.Encoded());
  uint64_t seq = 0;
  std::string_view batch;
  ASSERT_OK(DecodeWalPayload(payload, &seq, &batch));
  EXPECT_EQ(seq, 0x0102030405060708ull);
  EXPECT_EQ(batch, b.Encoded());
}

TEST(WalPayloadTest, TooShortIsCorruption) {
  uint64_t seq = 0;
  std::string_view batch;
  const std::string payload = EncodeWalPayloadHeader(1) + "abc";  // 3-byte batch header
  EXPECT_TRUE(DecodeWalPayload(payload, &seq, &batch).IsCorruption());
}

}  // namespace
}  // namespace lsmkv
