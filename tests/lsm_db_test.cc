// Durability and recovery behaviour of the engine, on the fault-injection Env.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "db/batch_format.h"
#include "db/filenames.h"
#include "env/fault_injection_env.h"
#include "lsmkv/db.h"
#include "test_util.h"
#include "wal/wal.h"

namespace lsmkv {
namespace {

using Mode = FaultInjectionEnv::PowerLossMode;

WriteOptions Synced() {
  WriteOptions o;
  o.sync = true;
  return o;
}

class LsmDBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.create_if_missing = true;
    options_.env = &env_;
    ASSERT_OK(Reopen());
  }

  Status Reopen() {
    db_.reset();
    return DB::Open(options_, "/db", &db_);
  }

  // Closes the DB, loses power, and reopens.
  Status PowerLossAndReopen(Mode mode = Mode::kDropUnsynced, uint64_t seed = 1) {
    db_.reset();
    env_.SimulatePowerLoss(mode, seed);
    return Reopen();
  }

  std::string Get(std::string_view key) {
    std::string value;
    Status s = db_->Get(ReadOptions(), key, &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return s.ToString();
    return value;
  }

  std::unique_ptr<Directory> Dir() {
    std::unique_ptr<Directory> dir;
    EXPECT_OK(env_.OpenDirectory("/db", &dir));
    return dir;
  }

  std::vector<uint64_t> WalNumbers() {
    std::vector<std::string> names;
    EXPECT_OK(Dir()->List(&names));
    std::vector<uint64_t> wals;
    for (const auto& n : names) {
      if (auto number = ParseWalFileName(n)) wals.push_back(*number);
    }
    std::sort(wals.begin(), wals.end());
    return wals;
  }

  // Replaces a file's contents, durably.
  void Rewrite(const std::string& name, std::string_view contents) {
    auto dir = Dir();
    ASSERT_OK(dir->Remove(name));
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(dir->Create(name, &f));
    ASSERT_OK(f->Append(contents));
    ASSERT_OK(f->Sync());
    ASSERT_OK(dir->Sync());
  }

  std::string ReadWal(uint64_t number) {
    std::string contents;
    EXPECT_OK(ReadEntireFile(*Dir(), WalFileName(number), &contents));
    return contents;
  }

  // Writes a WAL file holding one record with the given payload.
  void WriteWal(uint64_t number, std::string_view payload) {
    auto dir = Dir();
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(dir->Create(WalFileName(number), &f));
    WalWriter w(std::move(f));
    ASSERT_OK(w.AddRecord({payload}));
    ASSERT_OK(w.Sync());
    ASSERT_OK(dir->Sync());
  }

  FaultInjectionEnv env_;
  Options options_;
  std::unique_ptr<DB> db_;
};

TEST_F(LsmDBTest, CleanReopenKeepsEveryWrite) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "b", "2"));
  ASSERT_OK(db_->Delete(WriteOptions(), "a"));
  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  // Sequence numbers carry on from the recovered ones: a new write wins.
  ASSERT_OK(db_->Put(WriteOptions(), "b", "3"));
  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("b"), "3");
}

TEST_F(LsmDBTest, SyncedWriteSurvivesPowerLoss) {
  ASSERT_OK(db_->Put(Synced(), "k", "v"));
  ASSERT_OK(PowerLossAndReopen());
  EXPECT_EQ(Get("k"), "v");
}

TEST_F(LsmDBTest, UnsyncedWriteCanBeLostOnPowerLoss) {
  ASSERT_OK(db_->Put(Synced(), "kept", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "lost", "2"));
  ASSERT_OK(PowerLossAndReopen(Mode::kDropUnsynced));
  EXPECT_EQ(Get("kept"), "1");
  EXPECT_EQ(Get("lost"), "NOT_FOUND");
}

TEST_F(LsmDBTest, SyncedWriteMakesEarlierUnsyncedWritesDurable) {
  ASSERT_OK(db_->Put(WriteOptions(), "early", "1"));
  ASSERT_OK(db_->Put(Synced(), "late", "2"));
  ASSERT_OK(PowerLossAndReopen());
  EXPECT_EQ(Get("early"), "1");
  EXPECT_EQ(Get("late"), "2");
}

// D10: rotation syncs the old WAL, so a synced write in the new WAL can't
// survive a power loss that the earlier, unsynced writes don't.
TEST_F(LsmDBTest, RotationKeepsRecoveryAPrefix) {
  options_.memtable_size = 4096;
  ASSERT_OK(Reopen());
  const size_t wals_before = WalNumbers().size();
  int unsynced = 0;
  while (WalNumbers().size() == wals_before) {
    ASSERT_OK(db_->Put(WriteOptions(), "u" + std::to_string(unsynced), std::string(200, 'x')));
    ++unsynced;
  }
  ASSERT_OK(db_->Put(Synced(), "synced", "after rotation"));
  ASSERT_OK(PowerLossAndReopen());
  EXPECT_EQ(Get("synced"), "after rotation");
  for (int i = 0; i < unsynced; ++i) EXPECT_EQ(Get("u" + std::to_string(i)), std::string(200, 'x'));
}

TEST_F(LsmDBTest, ReadsSpanSealedMemtables) {
  options_.memtable_size = 4096;
  ASSERT_OK(Reopen());
  for (int i = 0; i < 500; ++i) ASSERT_OK(db_->Put(WriteOptions(), "k" + std::to_string(i % 50), std::to_string(i)));
  ASSERT_OK(db_->Delete(WriteOptions(), "k7"));
  EXPECT_GT(WalNumbers().size(), 3u);
  EXPECT_EQ(Get("k3"), "453");
  EXPECT_EQ(Get("k7"), "NOT_FOUND");
  auto it = db_->NewIterator(ReadOptions());
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  ASSERT_OK(it->status());
  EXPECT_EQ(n, 49);
}

TEST_F(LsmDBTest, TornTailIsDroppedAndTruncated) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "b", "2"));
  db_.reset();
  const uint64_t wal = WalNumbers().back();
  const std::string contents = ReadWal(wal);
  ASSERT_OK(Dir()->Truncate(WalFileName(wal), contents.size() - 3));  // tear the last record

  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
  // The torn bytes are gone for good, so the next open, when this WAL is no
  // longer the newest, doesn't mistake them for corruption.
  EXPECT_LT(ReadWal(wal).size(), contents.size() - 3);
  ASSERT_OK(db_->Put(WriteOptions(), "c", "3"));
  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("c"), "3");
}

TEST_F(LsmDBTest, DamageInAnOlderWalIsCorruption) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(Reopen());  // "a" is now in an older WAL
  ASSERT_OK(db_->Put(WriteOptions(), "b", "2"));
  db_.reset();
  const uint64_t older = WalNumbers().front();
  std::string contents = ReadWal(older);
  contents.back() ^= 0x01;
  Rewrite(WalFileName(older), contents);
  Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(LsmDBTest, DamageInTheNewestWalIsATornTail) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));
  ASSERT_OK(db_->Put(WriteOptions(), "b", "2"));
  db_.reset();
  const uint64_t newest = WalNumbers().back();
  std::string contents = ReadWal(newest);
  contents.back() ^= 0x01;
  Rewrite(WalFileName(newest), contents);
  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "NOT_FOUND");
}

TEST_F(LsmDBTest, WellChecksummedButUndecodableBatchIsCorruption) {
  db_.reset();
  WriteWal(WalNumbers().back() + 1, EncodeWalPayloadHeader(1) + std::string("\x01\x00\x00\x00\x7f", 5));
  Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(LsmDBTest, SequenceGapIsCorruption) {
  ASSERT_OK(db_->Put(WriteOptions(), "a", "1"));  // sequence 1
  db_.reset();
  WriteBatch b;
  b.Put("b", "2");
  WriteWal(WalNumbers().back() + 1, EncodeWalPayloadHeader(5) + std::string(b.Encoded()));
  Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_F(LsmDBTest, FailedSyncIsSticky) {
  ASSERT_OK(db_->Put(Synced(), "before", "1"));
  env_.FailFileSyncs(1);
  EXPECT_TRUE(db_->Put(Synced(), "failed", "2").IsIOError());
  // The file system works again, but the DB refuses writes until reopened.
  EXPECT_TRUE(db_->Put(WriteOptions(), "after", "3").IsIOError());
  EXPECT_EQ(Get("before"), "1");  // reads still work
  EXPECT_EQ(Get("after"), "NOT_FOUND");
  ASSERT_OK(Reopen());
  EXPECT_EQ(Get("before"), "1");
  ASSERT_OK(db_->Put(WriteOptions(), "after", "3"));
}

TEST_F(LsmDBTest, EmptyBatchWritesNothing) {
  const std::string before = ReadWal(WalNumbers().back());
  ASSERT_OK(db_->Write(Synced(), WriteBatch()));
  EXPECT_EQ(ReadWal(WalNumbers().back()), before);
}

// Memtables rotate under readers: every read must still see whole batches and
// the newest value of each key it was written before.
TEST_F(LsmDBTest, ConcurrentReadsDuringRotation) {
  options_.memtable_size = 8192;
  ASSERT_OK(Reopen());
  constexpr int kBatches = 3000;
  std::atomic<bool> done{false};
  std::atomic<int> errors{0};

  std::thread writer([&] {
    for (int i = 1; i <= kBatches; ++i) {
      WriteBatch b;
      const std::string v = std::to_string(i);
      b.Put("a", v);
      b.Put("b", v);
      b.Put("pad" + std::to_string(i % 97), std::string(64, 'p'));
      if (!db_->Write(WriteOptions(), b).ok()) errors++;
    }
    done = true;
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      int last_seen = 0;
      while (!done) {
        auto it = db_->NewIterator(ReadOptions());
        it->Seek("a");
        std::string a = "0", b = "0";  // before the first batch, neither key exists
        if (it->Valid() && it->key() == "a") {
          a = it->value();
          it->Next();
          if (it->Valid() && it->key() == "b") b = it->value();
        }
        if (a != b) errors++;  // torn batch
        std::string got;
        Status s = db_->Get(ReadOptions(), "a", &got);
        const int now = s.ok() ? std::stoi(got) : 0;
        if (now < last_seen) errors++;  // went back in time
        last_seen = now;
      }
    });
  }
  writer.join();
  for (auto& t : readers) t.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_GT(WalNumbers().size(), 10u);
  EXPECT_EQ(Get("a"), std::to_string(kBatches));
}

}  // namespace
}  // namespace lsmkv
