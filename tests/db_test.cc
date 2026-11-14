// Behaviour of the public DB API. Engine-agnostic: these tests must keep
// passing unchanged as the std::map stand-in is replaced by the real engine.

#include "lsmkv/db.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_util.h"

namespace lsmkv {
namespace {

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_OK(Reopen()); }

  Status Reopen() {
    db_.reset();
    Options options;
    options.create_if_missing = true;
    return DB::Open(options, DbPath(), &db_);
  }

  std::string DbPath() const { return dir_.path() + "/db"; }

  std::string Get(std::string_view key) {
    std::string value;
    Status s = db_->Get(ReadOptions(), key, &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return s.ToString();
    return value;
  }

  // All key=value pairs, in iteration order.
  std::string Contents(Iterator* it) {
    std::string out;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      out += std::string(it->key()) + "=" + std::string(it->value()) + ";";
    }
    EXPECT_OK(it->status());
    return out;
  }

  test::TempDir dir_;
  std::unique_ptr<DB> db_;
};

TEST_F(DBTest, OpenMissingWithoutCreateFails) {
  Options options;  // create_if_missing = false
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, dir_.path() + "/other", &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(db, nullptr);
}

TEST_F(DBTest, OpenExistingWithErrorIfExistsFails) {
  db_.reset();
  Options options;
  options.error_if_exists = true;
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, DbPath(), &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(DBTest, SecondOpenWhileOpenFails) {
  Options options;
  std::unique_ptr<DB> second;
  Status s = DB::Open(options, DbPath(), &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(second, nullptr);

  db_.reset();  // closing releases the LOCK
  ASSERT_OK(DB::Open(options, DbPath(), &second));
}

TEST_F(DBTest, PutGetOverwriteDelete) {
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  ASSERT_OK(db_->Put(WriteOptions(), "k", "v1"));
  EXPECT_EQ(Get("k"), "v1");
  ASSERT_OK(db_->Put(WriteOptions(), "k", "v2"));
  EXPECT_EQ(Get("k"), "v2");
  ASSERT_OK(db_->Delete(WriteOptions(), "k"));
  EXPECT_EQ(Get("k"), "NOT_FOUND");
}

TEST_F(DBTest, DeleteMissingKeyIsOk) {
  ASSERT_OK(db_->Delete(WriteOptions(), "never-written"));
}

TEST_F(DBTest, EmptyValueIsNotNotFound) {
  ASSERT_OK(db_->Put(WriteOptions(), "k", ""));
  std::string value = "garbage";
  ASSERT_OK(db_->Get(ReadOptions(), "k", &value));
  EXPECT_EQ(value, "");
}

TEST_F(DBTest, EmptyKeyAndEmbeddedNul) {
  std::string nul_key("a\0b", 3);
  std::string nul_value("x\0\0y", 4);
  ASSERT_OK(db_->Put(WriteOptions(), "", "empty-key"));
  ASSERT_OK(db_->Put(WriteOptions(), nul_key, nul_value));
  EXPECT_EQ(Get(""), "empty-key");
  EXPECT_EQ(Get(nul_key), nul_value);
  EXPECT_EQ(Get("a"), "NOT_FOUND");  // "a\0b" is not "a"
}

TEST_F(DBTest, KeysOrderAsUnsignedBytes) {
  // If char were compared as signed, "\x80" (-128) would sort before "\x7f".
  ASSERT_OK(db_->Put(WriteOptions(), "\x80", "hi"));
  ASSERT_OK(db_->Put(WriteOptions(), "\x7f", "lo"));
  ASSERT_OK(db_->Put(WriteOptions(), "\xff", "max"));
  ASSERT_OK(db_->Put(WriteOptions(), std::string_view("\0", 1), "min"));
  auto it = db_->NewIterator(ReadOptions());
  std::vector<std::string> values;
  for (it->SeekToFirst(); it->Valid(); it->Next()) values.emplace_back(it->value());
  EXPECT_EQ(values, (std::vector<std::string>{"min", "lo", "hi", "max"}));
}

TEST_F(DBTest, PrefixKeysSortBeforeExtensions) {
  ASSERT_OK(db_->Put(WriteOptions(), "ab", "2"));
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "b", "3"));
  auto it = db_->NewIterator(ReadOptions());
  EXPECT_EQ(Contents(it.get()), "a=1;ab=2;b=3;");
}

TEST_F(DBTest, BatchLastOperationWins) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "1");
  batch.Delete("a");
  batch.Put("b", "2");
  batch.Put("c", "1");
  ASSERT_OK(db_->Write(WriteOptions(), batch));
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("c"), "1");
}

TEST_F(DBTest, NewIteratorIsUnpositioned) {
  ASSERT_OK(db_->Put(WriteOptions(), "k", "v"));
  auto it = db_->NewIterator(ReadOptions());
  EXPECT_FALSE(it->Valid());
}

TEST_F(DBTest, IteratorSeek) {
  for (const char* k : {"b", "d", "f"}) ASSERT_OK(db_->Put(WriteOptions(), k, k));
  auto it = db_->NewIterator(ReadOptions());
  it->Seek("d");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "d");  // exact match
  it->Seek("c");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "d");  // next key up
  it->Seek("");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  it->Seek("g");
  EXPECT_FALSE(it->Valid());  // past the end
  EXPECT_OK(it->status());
}

TEST_F(DBTest, IteratorSeesSnapshotAtCreation) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "b", "1"));
  auto it = db_->NewIterator(ReadOptions());

  ASSERT_OK(db_->Put(WriteOptions(), "a", "2"));  // overwrite
  ASSERT_OK(db_->Delete(WriteOptions(), "b"));    // delete
  ASSERT_OK(db_->Put(WriteOptions(), "c", "2"));  // insert

  EXPECT_EQ(Contents(it.get()), "a=1;b=1;");
  auto fresh = db_->NewIterator(ReadOptions());
  EXPECT_EQ(Contents(fresh.get()), "a=2;c=2;");
}

TEST_F(DBTest, ManyIteratorsPinIndependentSnapshots) {
  std::vector<std::unique_ptr<Iterator>> its;
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(db_->Put(WriteOptions(), "k" + std::to_string(i), "v"));
    its.push_back(db_->NewIterator(ReadOptions()));
  }
  for (int i = 0; i < 5; ++i) {
    int count = 0;
    for (its[i]->SeekToFirst(); its[i]->Valid(); its[i]->Next()) ++count;
    EXPECT_EQ(count, i + 1);
  }
}

// Writers commit batches that set "a" and "b" to the same value. A reader
// reading both from one iterator snapshot must never see them differ: that
// would mean a torn batch or a torn snapshot. Run under TSan to check locking.
TEST_F(DBTest, ConcurrentBatchesAreAtomicToReaders) {
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kBatchesPerWriter = 2000;
  {
    WriteBatch init;
    init.Put("a", "0");
    init.Put("b", "0");
    ASSERT_OK(db_->Write(WriteOptions(), init));
  }

  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  std::atomic<int> write_errors{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kBatchesPerWriter; ++i) {
        std::string v = std::to_string(w) + ":" + std::to_string(i);
        WriteBatch batch;
        batch.Put("a", v);
        batch.Put("b", v);
        batch.Put("w" + std::to_string(w), v);
        if (!db_->Write(WriteOptions(), batch).ok()) write_errors++;
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&] {
      while (!done.load(std::memory_order_relaxed)) {
        auto it = db_->NewIterator(ReadOptions());
        it->Seek("a");
        std::string a = it->Valid() ? std::string(it->value()) : "";
        it->Next();
        std::string b = it->Valid() ? std::string(it->value()) : "";
        if (a != b) torn++;
      }
    });
  }
  for (int w = 0; w < kWriters; ++w) threads[w].join();
  done.store(true, std::memory_order_relaxed);
  for (size_t t = kWriters; t < threads.size(); ++t) threads[t].join();

  EXPECT_EQ(torn.load(), 0);
  EXPECT_EQ(write_errors.load(), 0);
  for (int w = 0; w < kWriters; ++w) {
    EXPECT_EQ(Get("w" + std::to_string(w)), std::to_string(w) + ":" + std::to_string(kBatchesPerWriter - 1));
  }
}

}  // namespace
}  // namespace lsmkv
