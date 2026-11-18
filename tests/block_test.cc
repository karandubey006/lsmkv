#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "table/block_reader.h"
#include "table/block_writer.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

using test::Printable;
using Entries = std::vector<std::pair<std::string, std::string>>;

std::string IKey(std::string_view user_key, SequenceNumber seq,
                 ValueType type = ValueType::kValue) {
  std::string key;
  AppendInternalKey(&key, user_key, seq, type);
  return key;
}

// Sorted entries whose user keys share long prefixes, with a few binary and
// empty keys and several versions of some user keys.
Entries SampleEntries(int n) {
  Entries entries;
  entries.emplace_back(IKey("", 5), "empty-key-value");
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user/key/%06d", i);
    if (i % 7 == 0) {
      // Three versions, newest first.
      entries.emplace_back(IKey(buf, 300), "v300-" + std::to_string(i));
      entries.emplace_back(IKey(buf, 200, ValueType::kDeletion), "");
      entries.emplace_back(IKey(buf, 100), "v100-" + std::to_string(i));
    } else {
      entries.emplace_back(IKey(buf, static_cast<SequenceNumber>(i + 1)),
                           std::string(static_cast<size_t>(i % 13), 'v'));
    }
  }
  entries.emplace_back(IKey(std::string("\xff\x00\x01", 3), 9), std::string("\0bin\0", 5));
  return entries;
}

std::string Build(const Entries& entries, int restart_interval) {
  BlockWriter writer(restart_interval);
  for (const auto& [k, v] : entries) writer.Add(k, v);
  const size_t estimate = writer.EstimatedSize();
  std::string block(writer.Finish());
  EXPECT_EQ(block.size(), estimate);
  return block;
}

std::shared_ptr<const BlockReader> ParseOk(std::string contents) {
  std::unique_ptr<BlockReader> block;
  Status s = BlockReader::Parse(std::move(contents), &block);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return block;
}

Entries ReadAll(Iterator& it) {
  Entries out;
  for (it.SeekToFirst(); it.Valid(); it.Next()) out.emplace_back(it.key(), it.value());
  return out;
}

class BlockRoundTrip : public ::testing::TestWithParam<int> {};

TEST_P(BlockRoundTrip, IterateAndSeek) {
  const Entries entries = SampleEntries(500);
  auto block = ParseOk(Build(entries, GetParam()));
  ASSERT_NE(block, nullptr);
  auto it = block->NewIterator();
  EXPECT_EQ(ReadAll(*it), entries);
  EXPECT_OK(it->status());

  for (size_t i = 0; i < entries.size(); ++i) {
    // Exactly at each key.
    it->Seek(entries[i].first);
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(it->key(), entries[i].first);
    EXPECT_EQ(it->value(), entries[i].second);

    // Just after it: the same user key with the next lower sequence number
    // sorts after it and before the next entry.
    const auto parts = DecodeInternalKey(entries[i].first);
    ASSERT_TRUE(parts.has_value());
    if (parts->sequence > 0) {
      it->Seek(IKey(parts->user_key, parts->sequence - 1, ValueType::kValue));
      if (i + 1 < entries.size()) {
        ASSERT_TRUE(it->Valid()) << i;
        EXPECT_EQ(it->key(), entries[i + 1].first) << i;
      } else {
        EXPECT_FALSE(it->Valid());
      }
    }
  }

  it->Seek(IKey("", kMaxSequenceNumber));  // before everything
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), entries.front().first);
  it->Seek(IKey("\xff\xff", kMaxSequenceNumber));  // past everything
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

INSTANTIATE_TEST_SUITE_P(RestartIntervals, BlockRoundTrip, ::testing::Values(1, 2, 16, 1000000));

TEST(BlockTest, EmptyBlock) {
  BlockWriter writer(16);
  EXPECT_TRUE(writer.empty());
  const std::string contents(writer.Finish());
  EXPECT_EQ(contents.size(), 8u);
  auto block = ParseOk(contents);
  ASSERT_NE(block, nullptr);
  auto it = block->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek(IKey("a", 1));
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

TEST(BlockTest, ResetReusesWriter) {
  BlockWriter writer(4);
  writer.Add(IKey("zzz", 1), "old");
  static_cast<void>(writer.Finish());
  writer.Reset();
  EXPECT_TRUE(writer.empty());
  const Entries entries = {{IKey("a", 1), "1"}, {IKey("b", 1), "2"}};
  for (const auto& [k, v] : entries) writer.Add(k, v);
  auto block = ParseOk(std::string(writer.Finish()));
  auto it = block->NewIterator();
  EXPECT_EQ(ReadAll(*it), entries);
}

TEST(BlockTest, OwningIteratorKeepsBlockAlive) {
  const Entries entries = SampleEntries(50);
  auto block = ParseOk(Build(entries, 16));
  auto it = BlockReader::NewIterator(std::move(block));
  block.reset();
  EXPECT_EQ(ReadAll(*it), entries);
}

TEST(BlockTest, PrefixCompressionShrinksBlock) {
  const Entries entries = SampleEntries(200);
  EXPECT_LT(Build(entries, 16).size(), Build(entries, 1).size());
}

std::string RawBlock(std::string_view entries, const std::vector<uint32_t>& restarts) {
  std::string b(entries);
  for (uint32_t r : restarts) PutFixed32(&b, r);
  PutFixed32(&b, static_cast<uint32_t>(restarts.size()));
  return b;
}

std::string RawEntry(uint32_t shared, std::string_view suffix, std::string_view value) {
  std::string e;
  PutVarint32(&e, shared);
  PutVarint32(&e, static_cast<uint32_t>(suffix.size()));
  PutVarint32(&e, static_cast<uint32_t>(value.size()));
  e.append(suffix);
  e.append(value);
  return e;
}

void ExpectParseCorruption(std::string contents) {
  std::unique_ptr<BlockReader> block;
  Status s = BlockReader::Parse(std::move(contents), &block);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(BlockTest, BadRestartArrays) {
  const std::string e1 = RawEntry(0, IKey("a", 1), "x");
  const std::string e2 = RawEntry(0, IKey("b", 1), "y");
  const auto u32 = static_cast<uint32_t>(e1.size());
  ExpectParseCorruption("");
  ExpectParseCorruption("abc");
  ExpectParseCorruption(RawBlock(e1, {}));                // zero restarts
  ExpectParseCorruption(RawBlock(e1 + e2, {4}));          // first restart not 0
  ExpectParseCorruption(RawBlock(e1 + e2, {0, 0}));       // not increasing
  ExpectParseCorruption(RawBlock(e1 + e2, {0, u32 + 99}));  // past the entries
  ExpectParseCorruption(RawBlock("", {0, 0}));            // empty block, two restarts
  std::string huge_count;
  PutFixed32(&huge_count, 0xFFFFFFFF);
  ExpectParseCorruption(e1 + huge_count);
}

// Parses contents (which must be accepted) and expects iteration to report
// Corruption.
void ExpectIterationCorruption(std::string contents) {
  auto block = ParseOk(std::move(contents));
  ASSERT_NE(block, nullptr);
  auto it = block->NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
  }
  EXPECT_TRUE(it->status().IsCorruption()) << it->status().ToString();
  // Sticky: seeking again doesn't revive it.
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST(BlockTest, MalformedEntries) {
  const std::string e1 = RawEntry(0, IKey("a", 1), "x");
  // Restart entry with shared != 0.
  ExpectIterationCorruption(RawBlock(RawEntry(1, IKey("a", 1), "x"), {0}));
  // shared longer than the previous key.
  ExpectIterationCorruption(RawBlock(e1 + RawEntry(100, "", "y"), {0}));
  // Key shorter than an internal key's trailer.
  ExpectIterationCorruption(RawBlock(RawEntry(0, "short", "x"), {0}));
  // Unknown value type in the trailer.
  ExpectIterationCorruption(RawBlock(RawEntry(0, "a" + std::string(8, '\x07'), "x"), {0}));
  // Value length runs past the entries.
  std::string bad_len;
  PutVarint32(&bad_len, 0);
  PutVarint32(&bad_len, 9);
  PutVarint32(&bad_len, 1000);
  ExpectIterationCorruption(RawBlock(e1 + bad_len + IKey("b", 1), {0}));
  // A restart offset that lands inside an entry.
  const std::string e2 = RawEntry(0, IKey("b", 1), "y");
  ExpectIterationCorruption(RawBlock(
      e1 + e2 + RawEntry(0, IKey("c", 1), "z"),
      {0, static_cast<uint32_t>(e1.size() + 1), static_cast<uint32_t>(e1.size() + e2.size())}));
}

TEST(BlockTest, SeekReportsBadRestartKey) {
  const std::string e1 = RawEntry(0, IKey("a", 1), "x");
  const std::string bad = RawEntry(0, "tiny", "y");
  auto block = ParseOk(RawBlock(e1 + bad, {0, static_cast<uint32_t>(e1.size())}));
  auto it = block->NewIterator();
  it->Seek(IKey("b", 1));
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

// Runs every iterator operation over arbitrary contents. Under ASan this
// checks that no input makes the reader touch memory outside the block.
// Returns the number of entries a full scan decoded (0 if Parse rejected it).
int Exercise(std::string contents, std::mt19937& rng) {
  std::unique_ptr<BlockReader> parsed;
  if (!BlockReader::Parse(std::move(contents), &parsed).ok()) return 0;
  std::shared_ptr<const BlockReader> block = std::move(parsed);
  auto it = block->NewIterator();
  int steps = 0;
  for (it->SeekToFirst(); it->Valid() && steps < 10000; it->Next(), ++steps) {
    static_cast<void>(it->key());
    static_cast<void>(it->value());
  }
  static_cast<void>(it->status());
  for (int i = 0; i < 4; ++i) {
    auto seek_it = block->NewIterator();
    std::string user(rng() % 6, '\0');
    for (char& c : user) c = static_cast<char>(rng());
    seek_it->Seek(IKey(user, rng() % 1000));
    if (seek_it->Valid()) seek_it->Next();
  }
  return steps;
}

TEST(BlockTest, FuzzRandomBytes) {
  std::mt19937 rng(1234);
  int decoded = 0;
  auto random_bytes = [&rng](size_t n) {
    std::string out(n, '\0');
    for (char& c : out) c = static_cast<char>(rng());
    return out;
  };
  for (int iter = 0; iter < 4000; ++iter) {
    std::string contents;
    if (iter % 2 == 0) {
      contents = random_bytes(rng() % 200);
    } else {
      // Structurally plausible entries with random fields, so decoding gets
      // past the header checks.
      const int n = static_cast<int>(rng() % 8);
      for (int e = 0; e < n; ++e) {
        std::string suffix = random_bytes(rng() % 14);
        if (suffix.size() >= 8) suffix[suffix.size() - 8] = static_cast<char>(rng() % 3);
        contents += RawEntry(rng() % 4, suffix, random_bytes(rng() % 4));
      }
      PutFixed32(&contents, 0);
      PutFixed32(&contents, 1);
    }
    // Often give it a plausible restart count so parsing gets further.
    if (contents.size() >= 8 && rng() % 2 == 0) {
      const uint32_t n = 1 + rng() % 3;
      if ((n + 1) * 4 <= contents.size()) {
        EncodeFixed32(contents.data() + contents.size() - 4, n);
        EncodeFixed32(contents.data() + contents.size() - 4 * (n + 1), 0);
      }
    }
    decoded += Exercise(std::move(contents), rng);
  }
  // Some random inputs must get past Parse and decode, or this tests little.
  EXPECT_GT(decoded, 0);
}

TEST(BlockTest, FuzzMutatedBlocks) {
  std::mt19937 rng(99);
  int decoded = 0;
  const std::string valid = Build(SampleEntries(40), 4);
  for (int iter = 0; iter < 4000; ++iter) {
    std::string contents = valid;
    const int flips = 1 + static_cast<int>(rng() % 4);
    for (int f = 0; f < flips; ++f) contents[rng() % contents.size()] = static_cast<char>(rng());
    if (rng() % 4 == 0) contents.resize(rng() % contents.size());
    decoded += Exercise(std::move(contents), rng);
  }
  EXPECT_GT(decoded, 4000);
}

}  // namespace
}  // namespace lsmkv
