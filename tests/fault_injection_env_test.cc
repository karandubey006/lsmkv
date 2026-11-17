#include "env/fault_injection_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

#include "test_util.h"

namespace lsmkv {
namespace {

using Mode = FaultInjectionEnv::PowerLossMode;

class FaultInjectionEnvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK(env_.CreateDirectory("/db"));
    Reopen();
  }

  // Opens a fresh Directory handle, as a restarted process would.
  void Reopen() { ASSERT_OK(env_.OpenDirectory("/db", &dir_)); }

  void PowerLoss(Mode mode = Mode::kDropUnsynced, uint64_t seed = 1) {
    env_.SimulatePowerLoss(mode, seed);
    Reopen();
  }

  // Returns the file's contents, or nullopt if it doesn't exist.
  std::optional<std::string> Read(std::string_view name) {
    std::unique_ptr<ReadableFile> f;
    Status s = dir_->Open(name, &f);
    if (s.IsNotFound()) return std::nullopt;
    EXPECT_OK(s);
    uint64_t size = 0;
    EXPECT_OK(f->Size(&size));
    std::string scratch(size, '\0');
    std::string_view out;
    EXPECT_OK(f->ReadAt(0, size, &out, scratch.data()));
    return std::string(out);
  }

  std::unique_ptr<WritableFile> Create(std::string_view name) {
    std::unique_ptr<WritableFile> f;
    EXPECT_OK(dir_->Create(name, &f));
    return f;
  }

  FaultInjectionEnv env_;
  std::unique_ptr<Directory> dir_;
};

TEST_F(FaultInjectionEnvTest, SyncedFileInUnsyncedDirectoryVanishes) {
  auto f = Create("000001.log");
  ASSERT_OK(f->Append("acknowledged"));
  ASSERT_OK(f->Sync());  // the data is durable, but its name isn't
  PowerLoss();
  EXPECT_EQ(Read("000001.log"), std::nullopt);
}

TEST_F(FaultInjectionEnvTest, SyncedFileInSyncedDirectorySurvives) {
  auto f = Create("000001.log");
  ASSERT_OK(dir_->Sync());
  ASSERT_OK(f->Append("acknowledged"));
  ASSERT_OK(f->Sync());
  PowerLoss();
  EXPECT_EQ(Read("000001.log"), "acknowledged");
}

TEST_F(FaultInjectionEnvTest, UnsyncedBytesAreDropped) {
  auto f = Create("log");
  ASSERT_OK(dir_->Sync());
  ASSERT_OK(f->Append("synced|"));
  ASSERT_OK(f->Sync());
  ASSERT_OK(f->Append("unsynced"));
  EXPECT_EQ(Read("log"), "synced|unsynced");  // visible before the crash
  PowerLoss();
  EXPECT_EQ(Read("log"), "synced|");
}

TEST_F(FaultInjectionEnvTest, DirectorySyncDoesNotSyncFileData) {
  auto f = Create("log");
  ASSERT_OK(f->Append("data"));
  ASSERT_OK(dir_->Sync());
  PowerLoss();
  EXPECT_EQ(Read("log"), "");
}

TEST_F(FaultInjectionEnvTest, UnsyncedRenameAndRemoveAreUndone) {
  auto a = Create("a");
  ASSERT_OK(a->Append("A"));
  ASSERT_OK(a->Sync());
  auto b = Create("b");
  ASSERT_OK(b->Append("B"));
  ASSERT_OK(b->Sync());
  ASSERT_OK(dir_->Sync());

  ASSERT_OK(dir_->Rename("a", "renamed"));
  ASSERT_OK(dir_->Remove("b"));
  PowerLoss();
  EXPECT_EQ(Read("a"), "A");
  EXPECT_EQ(Read("b"), "B");
  EXPECT_EQ(Read("renamed"), std::nullopt);
}

TEST_F(FaultInjectionEnvTest, SyncedRenameIsDurable) {
  auto tmp = Create("CURRENT.tmp");
  ASSERT_OK(tmp->Append("MANIFEST-2"));
  ASSERT_OK(tmp->Sync());
  ASSERT_OK(dir_->Rename("CURRENT.tmp", "CURRENT"));
  ASSERT_OK(dir_->Sync());
  PowerLoss();
  EXPECT_EQ(Read("CURRENT"), "MANIFEST-2");
  EXPECT_EQ(Read("CURRENT.tmp"), std::nullopt);
}

TEST_F(FaultInjectionEnvTest, TruncateIsDurable) {
  auto f = Create("log");
  ASSERT_OK(dir_->Sync());
  ASSERT_OK(f->Append("good|torn"));
  ASSERT_OK(dir_->Truncate("log", 5));
  PowerLoss(Mode::kTornWrites, 7);
  EXPECT_EQ(Read("log"), "good|");
}

TEST_F(FaultInjectionEnvTest, CreatedDirectoriesAreDurable) {
  ASSERT_OK(env_.CreateDirectory("/other"));
  env_.SimulatePowerLoss(Mode::kDropUnsynced, 1);
  std::unique_ptr<Directory> d;
  EXPECT_OK(env_.OpenDirectory("/other", &d));
}

TEST_F(FaultInjectionEnvTest, HandlesFromBeforePowerLossAreDead) {
  auto f = Create("log");
  std::unique_ptr<ReadableFile> r;
  ASSERT_OK(dir_->Open("log", &r));
  std::unique_ptr<Directory> old_dir = std::move(dir_);

  PowerLoss();
  EXPECT_TRUE(f->Append("x").IsIOError());
  EXPECT_TRUE(f->Sync().IsIOError());
  char scratch[1];
  std::string_view out;
  EXPECT_TRUE(r->ReadAt(0, 1, &out, scratch).IsIOError());
  std::unique_ptr<WritableFile> g;
  EXPECT_TRUE(old_dir->Create("new", &g).IsIOError());
  EXPECT_TRUE(old_dir->Sync().IsIOError());
}

TEST_F(FaultInjectionEnvTest, PowerLossReleasesLocks) {
  std::unique_ptr<FileLock> old_lock;
  ASSERT_OK(dir_->Lock("LOCK", &old_lock));
  PowerLoss();

  std::unique_ptr<FileLock> new_lock;
  ASSERT_OK(dir_->Lock("LOCK", &new_lock));
  old_lock.reset();  // must not release the new holder's lock
  std::unique_ptr<FileLock> third;
  EXPECT_TRUE(dir_->Lock("LOCK", &third).IsIOError());
}

// Writes `synced` (synced) then `unsynced` (not) to a fresh env, loses power
// in kTornWrites mode and returns what survived.
std::string SurvivorAfterTornWrite(const std::string& synced, const std::string& unsynced,
                                   uint64_t seed) {
  FaultInjectionEnv env;
  EXPECT_OK(env.CreateDirectory("/db"));
  std::unique_ptr<Directory> dir;
  EXPECT_OK(env.OpenDirectory("/db", &dir));
  std::unique_ptr<WritableFile> f;
  EXPECT_OK(dir->Create("log", &f));
  EXPECT_OK(dir->Sync());
  EXPECT_OK(f->Append(synced));
  EXPECT_OK(f->Sync());
  EXPECT_OK(f->Append(unsynced));

  env.SimulatePowerLoss(Mode::kTornWrites, seed);
  EXPECT_OK(env.OpenDirectory("/db", &dir));
  std::unique_ptr<ReadableFile> r;
  EXPECT_OK(dir->Open("log", &r));
  uint64_t size = 0;
  EXPECT_OK(r->Size(&size));
  std::string buf(size, '\0');
  std::string_view got;
  EXPECT_OK(r->ReadAt(0, size, &got, buf.data()));
  return std::string(got);
}

TEST(FaultInjectionTornWrites, PreserveSyncedBytesAndMangleOnlyWholePages) {
  const std::string synced(5000, 's');  // ends partway through page 1
  std::string unsynced(3 * 4096, '\0');
  for (size_t i = 0; i < unsynced.size(); ++i) unsynced[i] = static_cast<char>('a' + i % 26);
  const std::string full = synced + unsynced;

  bool saw_nothing_kept = false, saw_partial = false, saw_zero_tail_at_full_length = false,
       saw_hole_before_data = false;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    const std::string got = SurvivorAfterTornWrite(synced, unsynced, seed);
    ASSERT_EQ(got, SurvivorAfterTornWrite(synced, unsynced, seed)) << "not deterministic, seed " << seed;
    ASSERT_GE(got.size(), synced.size()) << "seed " << seed;
    ASSERT_LE(got.size(), full.size()) << "seed " << seed;
    ASSERT_EQ(got.substr(0, synced.size()), synced) << "seed " << seed;

    // Each 4 KB page of the unsynced part is either intact or all zeros.
    bool zero_page_seen = false, last_page_zeroed = false;
    for (size_t off = synced.size(); off < got.size();) {
      const size_t end = std::min(got.size(), (off / 4096 + 1) * 4096);
      const std::string_view page = std::string_view(got).substr(off, end - off);
      const bool intact = page == std::string_view(full).substr(off, end - off);
      const bool zeroed = std::all_of(page.begin(), page.end(), [](char c) { return c == 0; });
      ASSERT_TRUE(intact || zeroed) << "seed " << seed << " offset " << off;
      if (intact && zero_page_seen) saw_hole_before_data = true;
      zero_page_seen |= zeroed;
      last_page_zeroed = zeroed;
      off = end;
    }
    saw_nothing_kept |= got.size() == synced.size();
    saw_partial |= got.size() > synced.size() && got.size() < full.size();
    saw_zero_tail_at_full_length |= got.size() == full.size() && last_page_zeroed;
  }
  // The mode really produces each shape the WAL reader must survive.
  EXPECT_TRUE(saw_nothing_kept);
  EXPECT_TRUE(saw_partial);
  EXPECT_TRUE(saw_zero_tail_at_full_length);
  EXPECT_TRUE(saw_hole_before_data);  // a lost page followed by a surviving one
}

}  // namespace
}  // namespace lsmkv
