#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "env/env.h"
#include "env/fault_injection_env.h"
#include "table/block_writer.h"
#include "table/format.h"
#include "table/sst_reader.h"
#include "table/sst_writer.h"
#include "test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"

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

struct InternalKeyLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return CompareInternalKeys(a, b) < 0;
  }
};
using Model = std::map<std::string, std::string, InternalKeyLess>;

Entries SampleEntries(int n) {
  Entries entries;
  entries.emplace_back(IKey("", 5), "empty-key-value");
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i);
    if (i % 5 == 0) {
      entries.emplace_back(IKey(buf, 900), "new-" + std::to_string(i));
      entries.emplace_back(IKey(buf, 800, ValueType::kDeletion), "");
      entries.emplace_back(IKey(buf, 700), "old-" + std::to_string(i));
    } else {
      entries.emplace_back(
          IKey(buf, static_cast<SequenceNumber>(i + 1)),
          std::string(static_cast<size_t>(i % 37), static_cast<char>('a' + i % 26)));
    }
  }
  entries.emplace_back(IKey(std::string("\xfe\x00\xff", 3), 1), std::string("\0\1\2", 3));
  return entries;
}

// The next internal key after `key` in sort order that no table holds: the
// same user key at the next lower sequence number.
std::string JustAfter(std::string_view key) {
  const auto parts = DecodeInternalKey(key);
  EXPECT_TRUE(parts.has_value());
  EXPECT_GT(parts->sequence, 0u);
  return IKey(parts->user_key, parts->sequence - 1, ValueType::kValue);
}

// A WritableFile whose Appends start failing after a given number succeed.
class FailingFile final : public WritableFile {
 public:
  explicit FailingFile(int ok_appends) : ok_appends_(ok_appends) {}
  Status Append(std::string_view data) override {
    ++appends_;
    if (ok_appends_-- <= 0) return Status::IOError("injected append failure");
    bytes_ += data.size();
    return Status::OK();
  }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
  int appends() const { return appends_; }
  uint64_t bytes() const { return bytes_; }

 private:
  int ok_appends_;
  int appends_ = 0;
  uint64_t bytes_ = 0;
};

class TableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK(env_.CreateDirectory("/db"));
    ASSERT_OK(env_.OpenDirectory("/db", &dir_));
  }

  void WriteTable(const std::string& name, const Entries& entries,
                  const SstWriterOptions& options = {}) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(dir_->Create(name, &file));
    SstWriter writer(options, file.get());
    for (const auto& [k, v] : entries) ASSERT_OK(writer.Add(k, v));
    ASSERT_OK(writer.Finish());
    EXPECT_EQ(writer.NumEntries(), entries.size());
    if (!entries.empty()) {
      EXPECT_EQ(writer.smallest_key(), entries.front().first);
      EXPECT_EQ(writer.largest_key(), entries.back().first);
    }
    num_data_blocks_ = writer.NumDataBlocks();
    file_size_ = writer.FileSize();
    ASSERT_OK(file->Close());
    std::string contents;
    ASSERT_OK(ReadEntireFile(*dir_, name, &contents));
    EXPECT_EQ(contents.size(), file_size_);
  }

  Status OpenTable(const std::string& name, std::unique_ptr<SstReader>* table) {
    std::unique_ptr<ReadableFile> file;
    Status s = dir_->Open(name, &file);
    if (!s.ok()) return s;
    uint64_t size = 0;
    s = file->Size(&size);
    if (!s.ok()) return s;
    return SstReader::Open(std::move(file), size, table);
  }

  std::string Contents(const std::string& name) {
    std::string contents;
    EXPECT_OK(ReadEntireFile(*dir_, name, &contents));
    return contents;
  }

  void WriteRaw(const std::string& name, std::string_view contents) {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(dir_->Create(name, &file));
    ASSERT_OK(file->Append(contents));
    ASSERT_OK(file->Close());
  }

  std::vector<BlockHandle> DataBlockHandles(const SstReader& table) {
    std::vector<BlockHandle> handles;
    auto it = table.NewIndexIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      BlockHandle h;
      std::string_view v = it->value();
      EXPECT_OK(h.DecodeFrom(&v));
      handles.push_back(h);
    }
    EXPECT_OK(it->status());
    return handles;
  }

  FaultInjectionEnv env_;
  std::unique_ptr<Directory> dir_;
  uint64_t num_data_blocks_ = 0;
  uint64_t file_size_ = 0;
};

Entries Scan(const SstReader& table, Status* status = nullptr) {
  Entries out;
  auto it = table.NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) out.emplace_back(it->key(), it->value());
  if (status != nullptr) {
    *status = it->status();
  } else {
    EXPECT_OK(it->status());
  }
  return out;
}

// --- Round trips over a grid of block sizes and restart intervals ---------

class TableRoundTrip : public TableTest,
                       public ::testing::WithParamInterface<std::tuple<size_t, int>> {};

TEST_P(TableRoundTrip, ScanAndSeek) {
  const auto [block_size, restart_interval] = GetParam();
  const Entries entries = SampleEntries(1500);
  WriteTable("t.sst", entries, {block_size, restart_interval});
  if (block_size == 1) {
    EXPECT_EQ(num_data_blocks_, entries.size());
  }
  if (block_size >= (1 << 20)) {
    EXPECT_EQ(num_data_blocks_, 1u);
  }

  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("t.sst", &table));
  EXPECT_EQ(Scan(*table), entries);
  EXPECT_EQ(DataBlockHandles(*table).size(), num_data_blocks_);

  auto it = table->NewIterator();
  bool found;
  std::string key, value;
  for (size_t i = 0; i < entries.size(); ++i) {
    ASSERT_OK(table->SeekEntry(entries[i].first, &found, &key, &value));
    ASSERT_TRUE(found) << i;
    EXPECT_EQ(key, entries[i].first);
    EXPECT_EQ(value, entries[i].second);

    it->Seek(entries[i].first);
    ASSERT_TRUE(it->Valid()) << i;
    EXPECT_EQ(it->key(), entries[i].first);

    // Between this entry and the next.
    const std::string between = JustAfter(entries[i].first);
    ASSERT_OK(table->SeekEntry(between, &found, &key, &value));
    it->Seek(between);
    if (i + 1 < entries.size()) {
      ASSERT_TRUE(found) << i;
      EXPECT_EQ(key, entries[i + 1].first) << i;
      ASSERT_TRUE(it->Valid());
      EXPECT_EQ(it->key(), entries[i + 1].first);
    } else {
      EXPECT_FALSE(found);
      EXPECT_FALSE(it->Valid());
    }
  }

  ASSERT_OK(table->SeekEntry(IKey("", kMaxSequenceNumber), &found, &key, &value));
  ASSERT_TRUE(found);
  EXPECT_EQ(key, entries.front().first);
  ASSERT_OK(table->SeekEntry(IKey("\xff", kMaxSequenceNumber), &found, &key, &value));
  EXPECT_FALSE(found);
  it->Seek(IKey("\xff", kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

INSTANTIATE_TEST_SUITE_P(
    Grid, TableRoundTrip,
    ::testing::Values(std::make_tuple(size_t{1}, 16), std::make_tuple(size_t{1}, 1),
                      std::make_tuple(size_t{64}, 1), std::make_tuple(size_t{256}, 16),
                      std::make_tuple(size_t{4096}, 16), std::make_tuple(size_t{4096}, 1),
                      std::make_tuple(size_t{4096}, 1000000),
                      std::make_tuple(size_t{1} << 20, 16)));

TEST_F(TableTest, ValuesLargerThanBlockSize) {
  Entries entries;
  for (int i = 0; i < 20; ++i) {
    const size_t len = i % 3 == 0 ? 20000 : 10;
    entries.emplace_back(IKey("k" + std::to_string(100 + i), 1),
                         std::string(len, static_cast<char>('a' + i)));
  }
  entries.emplace_back(IKey("z", 1), std::string(1 << 20, 'z'));
  WriteTable("t.sst", entries, {4096, 16});
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("t.sst", &table));
  EXPECT_EQ(Scan(*table), entries);
  bool found;
  std::string key, value;
  ASSERT_OK(table->SeekEntry(IKey("z", 5), &found, &key, &value));
  ASSERT_TRUE(found);
  EXPECT_EQ(value.size(), size_t{1} << 20);
}

TEST_F(TableTest, ZeroEntryTable) {
  WriteTable("empty.sst", {});
  EXPECT_EQ(num_data_blocks_, 0u);
  EXPECT_EQ(file_size_, 8 + kBlockTrailerSize + kFooterSize);
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("empty.sst", &table));
  bool found = true;
  std::string key, value;
  ASSERT_OK(table->SeekEntry(IKey("a", 1), &found, &key, &value));
  EXPECT_FALSE(found);
  EXPECT_TRUE(Scan(*table).empty());
  auto it = table->NewIterator();
  it->Seek(IKey("", kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
  EXPECT_OK(it->status());
}

// The newest version visible at a snapshot must be found even when it is
// the first entry of the block after the one its predecessors end.
TEST_F(TableTest, VersionsAcrossBlockBoundaries) {
  Entries entries;
  entries.emplace_back(IKey("a", 50), "a");
  for (SequenceNumber seq = 20; seq >= 11; --seq) {
    entries.emplace_back(IKey("k", seq, seq % 4 == 0 ? ValueType::kDeletion : ValueType::kValue),
                         "k@" + std::to_string(seq));
  }
  entries.emplace_back(IKey("m", 3), "m");

  for (size_t block_size : {size_t{1}, size_t{30}, size_t{60}, size_t{100}, size_t{4096}}) {
    SCOPED_TRACE("block_size " + std::to_string(block_size));
    const std::string name = "v" + std::to_string(block_size) + ".sst";
    WriteTable(name, entries, {block_size, 2});
    std::unique_ptr<SstReader> table;
    ASSERT_OK(OpenTable(name, &table));
    for (SequenceNumber snapshot = 0; snapshot <= 25; ++snapshot) {
      bool found;
      std::string key, value;
      ASSERT_OK(table->SeekEntry(SeekKey("k", snapshot), &found, &key, &value));
      ASSERT_TRUE(found);
      const auto parts = DecodeInternalKey(key);
      ASSERT_TRUE(parts.has_value());
      if (snapshot < 11) {
        EXPECT_EQ(parts->user_key, "m");
      } else {
        EXPECT_EQ(parts->user_key, "k");
        EXPECT_EQ(parts->sequence, std::min<SequenceNumber>(snapshot, 20));
      }
    }
  }
}

TEST_F(TableTest, RealFileSystem) {
  test::TempDir tmp;
  std::unique_ptr<Directory> dir;
  ASSERT_OK(Env::Posix()->OpenDirectory(tmp.path(), &dir));
  const Entries entries = SampleEntries(300);
  {
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(dir->Create("000001.sst", &file));
    SstWriter writer({}, file.get());
    for (const auto& [k, v] : entries) ASSERT_OK(writer.Add(k, v));
    ASSERT_OK(writer.Finish());
    ASSERT_OK(file->Sync());
    ASSERT_OK(file->Close());
  }
  std::unique_ptr<ReadableFile> file;
  ASSERT_OK(dir->Open("000001.sst", &file));
  uint64_t size;
  ASSERT_OK(file->Size(&size));
  std::unique_ptr<SstReader> table;
  ASSERT_OK(SstReader::Open(std::move(file), size, &table));
  EXPECT_EQ(Scan(*table), entries);
}

// --- Write errors -----------------------------------------------------------

TEST_F(TableTest, WriteErrorsAreSticky) {
  const Entries entries = SampleEntries(200);
  FailingFile file(/*ok_appends=*/2);
  SstWriter writer({256, 16}, &file);
  Status first;
  for (const auto& [k, v] : entries) {
    Status s = writer.Add(k, v);
    if (!s.ok()) {
      first = s;
      break;
    }
  }
  ASSERT_TRUE(first.IsIOError()) << first.ToString();
  const int appends = file.appends();
  EXPECT_TRUE(writer.Add(IKey("zzzz", 1), "v").IsIOError());
  EXPECT_TRUE(writer.Finish().IsIOError());
  EXPECT_TRUE(writer.status().IsIOError());
  EXPECT_EQ(file.appends(), appends);  // nothing more was attempted
  EXPECT_EQ(writer.FileSize(), file.bytes());
}

TEST_F(TableTest, FooterWriteErrorReachesFinish) {
  FailingFile file(/*ok_appends=*/1);  // the index block succeeds, the footer fails
  SstWriter writer({}, &file);
  EXPECT_TRUE(writer.Finish().IsIOError());
}

TEST_F(TableTest, Abandon) {
  FailingFile file(/*ok_appends=*/100);
  SstWriter writer({}, &file);
  ASSERT_OK(writer.Add(IKey("a", 1), "v"));
  writer.Abandon();
  EXPECT_EQ(file.appends(), 0);
}

// --- Corruption -------------------------------------------------------------

TEST_F(TableTest, FlippedByteInEachDataBlock) {
  const Entries entries = SampleEntries(300);
  WriteTable("t.sst", entries, {512, 16});
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("t.sst", &table));
  const std::vector<BlockHandle> handles = DataBlockHandles(*table);
  ASSERT_GT(handles.size(), 5u);

  std::vector<std::string> last_keys;
  {
    auto it = table->NewIndexIterator();
    for (it->SeekToFirst(); it->Valid(); it->Next()) last_keys.emplace_back(it->key());
  }

  const std::string original = Contents("t.sst");
  for (size_t b = 0; b < handles.size(); ++b) {
    // A byte inside the contents, and a byte of the CRC trailer.
    for (uint64_t pos :
         {handles[b].offset + handles[b].size / 2, handles[b].offset + handles[b].size}) {
      SCOPED_TRACE("block " + std::to_string(b) + " byte " + std::to_string(pos));
      std::string damaged = original;
      damaged[pos] ^= 0x10;
      const std::string name = "bad" + std::to_string(b) + "_" + std::to_string(pos) + ".sst";
      WriteRaw(name, damaged);
      std::unique_ptr<SstReader> bad;
      ASSERT_OK(OpenTable(name, &bad));

      bool found;
      std::string key, value;
      EXPECT_TRUE(bad->SeekEntry(last_keys[b], &found, &key, &value).IsCorruption());
      EXPECT_FALSE(found);

      Status s;
      const Entries got = Scan(*bad, &s);
      EXPECT_TRUE(s.IsCorruption()) << s.ToString();
      // Everything before the damaged block came through intact.
      ASSERT_LE(got.size(), entries.size());
      EXPECT_TRUE(std::equal(got.begin(), got.end(), entries.begin()));

      auto it = bad->NewIterator();
      it->Seek(last_keys[b]);
      EXPECT_FALSE(it->Valid());
      EXPECT_TRUE(it->status().IsCorruption());
    }
  }
}

TEST_F(TableTest, FlippedByteInIndex) {
  WriteTable("t.sst", SampleEntries(300), {512, 16});
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("t.sst", &table));
  const BlockHandle index = table->footer().index;
  std::string damaged = Contents("t.sst");
  damaged[index.offset + index.size / 3] ^= 0x01;
  WriteRaw("bad.sst", damaged);
  std::unique_ptr<SstReader> bad;
  EXPECT_TRUE(OpenTable("bad.sst", &bad).IsCorruption());
}

TEST_F(TableTest, BadFooters) {
  WriteTable("t.sst", SampleEntries(50));
  const std::string original = Contents("t.sst");
  const size_t footer = original.size() - kFooterSize;
  std::unique_ptr<SstReader> table;

  auto expect_corrupt = [&](const std::string& name, const std::string& contents) {
    WriteRaw(name, contents);
    Status s = OpenTable(name, &table);
    EXPECT_TRUE(s.IsCorruption()) << name << ": " << s.ToString();
  };

  std::string bad_magic = original;
  bad_magic.back() ^= 0x01;
  expect_corrupt("magic.sst", bad_magic);

  std::string bad_version = original;
  EncodeFixed32(bad_version.data() + footer + 32, 2);
  expect_corrupt("version.sst", bad_version);

  for (size_t len : {size_t{0}, size_t{1}, kFooterSize - 1, kFooterSize, original.size() / 2,
                     original.size() - 1}) {
    expect_corrupt("trunc" + std::to_string(len) + ".sst", original.substr(0, len));
  }

  // Index handles that point past the footer, including ones whose sum
  // overflows.
  const std::vector<std::pair<uint64_t, uint64_t>> bad_handles = {
      {0, footer}, {footer, 1}, {footer - 2, 0}, {~uint64_t{0}, 1}, {1, ~uint64_t{0}},
      {~uint64_t{0} - 3, 8}};
  int n = 0;
  for (const auto& [offset, size] : bad_handles) {
    std::string damaged = original;
    EncodeFixed64(damaged.data() + footer, offset);
    EncodeFixed64(damaged.data() + footer + 8, size);
    expect_corrupt("index" + std::to_string(n++) + ".sst", damaged);
  }

  std::string bad_filter = original;
  EncodeFixed64(bad_filter.data() + footer + 16, 0);
  EncodeFixed64(bad_filter.data() + footer + 24, ~uint64_t{0});
  expect_corrupt("filter.sst", bad_filter);
}

// A hand-built table whose index points a data block past the index block.
TEST_F(TableTest, DataBlockHandlePastEnd) {
  int n = 0;
  for (const BlockHandle& bogus : {BlockHandle{1000, 10}, BlockHandle{0, 1u << 30},
                                   BlockHandle{~uint64_t{0}, ~uint64_t{0}}}) {
    const std::string name = "h" + std::to_string(n++) + ".sst";
    BlockWriter index(1);
    std::string handle;
    bogus.EncodeTo(&handle);
    index.Add(IKey("a", 1), handle);
    std::string file(index.Finish());
    Footer footer;
    footer.index = {0, file.size()};
    PutFixed32(&file, Crc32c(file));
    footer.EncodeTo(&file);
    WriteRaw(name, file);

    std::unique_ptr<SstReader> table;
    ASSERT_OK(OpenTable(name, &table));
    bool found;
    std::string key, value;
    EXPECT_TRUE(table->SeekEntry(IKey("a", 1), &found, &key, &value).IsCorruption());
    Status s;
    EXPECT_TRUE(Scan(*table, &s).empty());
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  }
}

TEST_F(TableTest, MalformedHandleInIndex) {
  BlockWriter index(1);
  index.Add(IKey("a", 1), std::string("\xff\xff", 2));  // truncated varint
  std::string file(index.Finish());
  Footer footer;
  footer.index = {0, file.size()};
  PutFixed32(&file, Crc32c(file));
  footer.EncodeTo(&file);
  WriteRaw("h.sst", file);
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("h.sst", &table));
  bool found;
  std::string key, value;
  EXPECT_TRUE(table->SeekEntry(IKey("a", 1), &found, &key, &value).IsCorruption());
}

// --- Randomized comparison with a std::map ---------------------------------

TEST_F(TableTest, RandomizedAgainstModel) {
  std::mt19937_64 rng(20261118);
  for (int round = 0; round < 12; ++round) {
    SCOPED_TRACE("round " + std::to_string(round));
    Model model;
    const int n = static_cast<int>(rng() % 2000);
    // A small key space so user keys get several versions.
    const int key_space = 1 + static_cast<int>(rng() % 400);
    for (int i = 0; i < n; ++i) {
      std::string user = "u" + std::to_string(rng() % key_space);
      if (rng() % 10 == 0) user.push_back('\0');
      const auto type = rng() % 4 == 0 ? ValueType::kDeletion : ValueType::kValue;
      const std::string value(rng() % 100, static_cast<char>('a' + rng() % 26));
      model[IKey(user, 1 + rng() % 1000, type)] = type == ValueType::kValue ? value : "";
    }
    const Entries entries(model.begin(), model.end());
    const SstWriterOptions options{1 + rng() % 3000, 1 + static_cast<int>(rng() % 20)};
    const std::string name = "r" + std::to_string(round) + ".sst";
    WriteTable(name, entries, options);
    std::unique_ptr<SstReader> table;
    ASSERT_OK(OpenTable(name, &table));
    ASSERT_EQ(Scan(*table), entries);

    auto it = table->NewIterator();
    for (int q = 0; q < 500; ++q) {
      const std::string target = IKey("u" + std::to_string(rng() % (key_space + 5)),
                                      rng() % 1100, ValueType::kValue);
      const auto expected = model.lower_bound(target);
      bool found;
      std::string key, value;
      ASSERT_OK(table->SeekEntry(target, &found, &key, &value));
      it->Seek(target);
      if (expected == model.end()) {
        EXPECT_FALSE(found) << Printable(target);
        EXPECT_FALSE(it->Valid());
        continue;
      }
      ASSERT_TRUE(found) << Printable(target);
      EXPECT_EQ(key, expected->first);
      EXPECT_EQ(value, expected->second);
      // A few steps forward from the seek match the model too.
      auto m = expected;
      for (int step = 0; step < 3 && m != model.end(); ++step, ++m) {
        ASSERT_TRUE(it->Valid());
        EXPECT_EQ(it->key(), m->first);
        EXPECT_EQ(it->value(), m->second);
        it->Next();
      }
    }
    EXPECT_OK(it->status());
  }
}

TEST_F(TableTest, ConcurrentReaders) {
  const Entries entries = SampleEntries(2000);
  WriteTable("t.sst", entries, {1024, 16});
  std::unique_ptr<SstReader> table;
  ASSERT_OK(OpenTable("t.sst", &table));

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      for (int i = 0; i < 500; ++i) {
        const auto& [k, v] = entries[rng() % entries.size()];
        bool found;
        std::string key, value;
        EXPECT_OK(table->SeekEntry(k, &found, &key, &value));
        EXPECT_TRUE(found);
        EXPECT_EQ(key, k);
        EXPECT_EQ(value, v);
      }
      EXPECT_EQ(Scan(*table), entries);
    });
  }
  for (auto& th : threads) th.join();
}

}  // namespace
}  // namespace lsmkv
