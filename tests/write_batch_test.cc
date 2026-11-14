#include "lsmkv/write_batch.h"

#include <gtest/gtest.h>

#include "db/write_batch_internal.h"
#include "test_util.h"

namespace lsmkv {
namespace {

// Renders a batch as "Put(k,v)Delete(k)..." to make order easy to assert.
struct Printer final : WriteBatch::Handler {
  std::string out;
  void Put(std::string_view k, std::string_view v) override {
    out += "Put(" + std::string(k) + "," + std::string(v) + ")";
  }
  void Delete(std::string_view k) override { out += "Delete(" + std::string(k) + ")"; }
};

std::string Print(const WriteBatch& b, Status* status = nullptr) {
  Printer p;
  Status s = b.Iterate(p);
  if (status != nullptr) *status = s;
  return p.out;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch b;
  EXPECT_EQ(b.Count(), 0u);
  Status s;
  EXPECT_EQ(Print(b, &s), "");
  EXPECT_TRUE(s.ok());
}

TEST(WriteBatchTest, PreservesOrder) {
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  b.Put("a", "2");
  EXPECT_EQ(b.Count(), 3u);
  EXPECT_EQ(Print(b), "Put(a,1)Delete(b)Put(a,2)");
}

TEST(WriteBatchTest, Clear) {
  WriteBatch b;
  b.Put("a", "1");
  b.Clear();
  EXPECT_EQ(b.Count(), 0u);
  EXPECT_EQ(Print(b), "");
  EXPECT_EQ(b.ApproximateSize(), WriteBatchInternal::kHeaderSize);
}

TEST(WriteBatchTest, BinaryKeysAndEmptyValues) {
  WriteBatch b;
  b.Put(std::string_view("k\0x", 3), "");
  b.Put("", "v");
  struct Collect final : WriteBatch::Handler {
    std::vector<std::pair<std::string, std::string>> puts;
    void Put(std::string_view k, std::string_view v) override { puts.emplace_back(k, v); }
    void Delete(std::string_view) override {}
  } c;
  ASSERT_OK(b.Iterate(c));
  ASSERT_EQ(c.puts.size(), 2u);
  EXPECT_EQ(c.puts[0].first, std::string("k\0x", 3));
  EXPECT_EQ(c.puts[0].second, "");
  EXPECT_EQ(c.puts[1].first, "");
}

TEST(WriteBatchTest, ContentsRoundTrip) {
  WriteBatch a;
  a.Put("x", "1");
  a.Delete("y");
  WriteBatch b;
  ASSERT_OK(WriteBatchInternal::SetContents(&b, WriteBatchInternal::Contents(a)));
  EXPECT_EQ(b.Count(), 2u);
  EXPECT_EQ(Print(b), "Put(x,1)Delete(y)");
}

TEST(WriteBatchTest, SetContentsRejectsShortHeader) {
  WriteBatch b;
  EXPECT_TRUE(WriteBatchInternal::SetContents(&b, "ab").IsCorruption());
}

TEST(WriteBatchTest, TruncatedRecordIsCorruption) {
  WriteBatch a;
  a.Put("key", "value");
  std::string rep(WriteBatchInternal::Contents(a));
  rep.pop_back();  // cut the last byte of the value
  WriteBatch b;
  ASSERT_OK(WriteBatchInternal::SetContents(&b, rep));
  Status s;
  Print(b, &s);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(WriteBatchTest, CountMismatchIsCorruption) {
  WriteBatch a;
  a.Put("k", "v");
  std::string rep(WriteBatchInternal::Contents(a));
  rep[0] = 2;  // header claims 2 records, body has 1
  WriteBatch b;
  ASSERT_OK(WriteBatchInternal::SetContents(&b, rep));
  Status s;
  Print(b, &s);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(WriteBatchTest, UnknownTagIsCorruption) {
  WriteBatch a;
  a.Put("k", "v");
  std::string rep(WriteBatchInternal::Contents(a));
  rep[WriteBatchInternal::kHeaderSize] = 0x7F;
  WriteBatch b;
  ASSERT_OK(WriteBatchInternal::SetContents(&b, rep));
  Status s;
  Print(b, &s);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

}  // namespace
}  // namespace lsmkv
