#include "db/manifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/filenames.h"
#include "env/fault_injection_env.h"
#include "test_util.h"
#include "wal/wal.h"

namespace lsmkv {
namespace {

using PowerLossMode = FaultInjectionEnv::PowerLossMode;

std::string IKey(std::string_view user_key, SequenceNumber seq = 1) {
  std::string key;
  AppendInternalKey(&key, user_key, seq, ValueType::kValue);
  return key;
}

// Everything a Manifest recovers, except next_file_number, which recovery
// may legitimately raise past the recorded value.
struct DbState {
  uint64_t log_number = 0;
  SequenceNumber last_sequence = 0;
  VersionEdit files;  // every file, level by level, in Version order

  bool operator==(const DbState&) const = default;
};

DbState StateOf(const Manifest& m) {
  DbState state{m.log_number(), m.last_sequence(), {}};
  const auto v = m.current();
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : v->files(level)) state.files.AddFile(level, *f);
  }
  return state;
}

std::string Describe(const DbState& s) {
  std::string out = "log=" + std::to_string(s.log_number) +
                    " seq=" + std::to_string(s.last_sequence) + " files=";
  for (const auto& a : s.files.added) {
    out += "L" + std::to_string(a.level) + ":" + std::to_string(a.info.number) + " ";
  }
  return out;
}

// The i-th edit of a deterministic history: flushes to L0, a move to L1,
// and counter changes.
VersionEdit HistoryEdit(Manifest& m, int i) {
  VersionEdit edit;
  const std::string lo(1, static_cast<char>('a' + 2 * i));
  const std::string hi(1, static_cast<char>('a' + 2 * i + 1));
  edit.AddFile(0, SstInfo{m.NewFileNumber(), 1000u + i, IKey(lo, 10 * i), IKey(hi, 10 * i)});
  edit.log_number = m.NewFileNumber();
  edit.last_sequence = 100 * (i + 1);
  if (i == 2) {
    // Move the first flush down a level, under the same number.
    const auto& l0 = m.current()->files(0);
    const SstInfo oldest = *l0.back();
    edit.RemoveFile(0, oldest.number);
    edit.AddFile(1, oldest);
  }
  return edit;
}

class ManifestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK(env_.CreateDirectory("/db"));
    Reopen();
  }

  // A fresh directory handle, as after a restart.
  void Reopen() { ASSERT_OK(env_.OpenDirectory("/db", &dir_)); }

  // Recovers into a new Manifest, which must find a database.
  std::unique_ptr<Manifest> Recovered() {
    auto m = std::make_unique<Manifest>(dir_.get());
    bool exists = false;
    EXPECT_OK(m->Recover(&exists));
    EXPECT_TRUE(exists);
    return m;
  }

  Status RecoverStatus() {
    Manifest m(dir_.get());
    bool exists = false;
    return m.Recover(&exists);
  }

  std::string Read(std::string_view name) {
    std::string contents;
    EXPECT_OK(ReadEntireFile(*dir_, name, &contents));
    return contents;
  }

  // Replaces a file's contents.
  void Overwrite(std::string_view name, std::string_view contents) {
    Status s = dir_->Remove(name);
    ASSERT_TRUE(s.ok() || s.IsNotFound());
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(dir_->Create(name, &f));
    ASSERT_OK(f->Append(contents));
    ASSERT_OK(f->Sync());
    ASSERT_OK(f->Close());
  }

  // Appends a well-checksummed record holding payload to the MANIFEST.
  void AppendRecord(uint64_t manifest_number, std::string_view payload) {
    const std::string name = ManifestFileName(manifest_number);
    const std::string old = Read(name);
    ASSERT_OK(dir_->Remove(name));
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(dir_->Create(name, &f));
    ASSERT_OK(f->Append(old));
    WalWriter writer(std::move(f));
    ASSERT_OK(writer.AddRecord({payload}));
    ASSERT_OK(writer.Sync());
  }

  FaultInjectionEnv env_;
  std::unique_ptr<Directory> dir_;
};

TEST(FileNamesTest, RoundTrip) {
  for (uint64_t n : {uint64_t{0}, uint64_t{7}, uint64_t{999999}, uint64_t{1234567}, UINT64_MAX}) {
    auto wal = ParseFileName(WalFileName(n));
    auto sst = ParseFileName(SstFileName(n));
    auto manifest = ParseFileName(ManifestFileName(n));
    ASSERT_TRUE(wal && sst && manifest);
    EXPECT_EQ(wal->kind, FileKind::kWal);
    EXPECT_EQ(sst->kind, FileKind::kSst);
    EXPECT_EQ(manifest->kind, FileKind::kManifest);
    EXPECT_EQ(wal->number, n);
    EXPECT_EQ(sst->number, n);
    EXPECT_EQ(manifest->number, n);
    EXPECT_EQ(ParseWalFileName(WalFileName(n)), n);
    EXPECT_EQ(ParseWalFileName(SstFileName(n)), std::nullopt);
  }
  EXPECT_EQ(SstFileName(42), "000042.sst");
  EXPECT_EQ(ManifestFileName(42), "MANIFEST-000042");
  EXPECT_EQ(ParseFileName("CURRENT")->kind, FileKind::kCurrent);
  EXPECT_EQ(ParseFileName("CURRENT.tmp")->kind, FileKind::kCurrentTmp);
  EXPECT_EQ(ParseFileName("LOCK")->kind, FileKind::kLock);
}

TEST(FileNamesTest, ForeignNames) {
  for (std::string_view name :
       {"", "current", "CURRENT.tmp2", "LOCK.old", "12345.sst", "0000042.sst", "000042.SST",
        "MANIFEST-", "MANIFEST-42", "MANIFEST-0000042", "MANIFEST-000042x", "manifest-000042",
        "+00042.wal", "-00042.sst", "000042", "000042.wal.tmp", "99999999999999999999.sst",
        ".sst", "MANIFEST--00042"}) {
    EXPECT_EQ(ParseFileName(name), std::nullopt) << name;
  }
}

TEST_F(ManifestTest, NoCurrentMeansNoDatabase) {
  Manifest m(dir_.get());
  bool exists = true;
  ASSERT_OK(m.Recover(&exists));
  EXPECT_FALSE(exists);
}

TEST_F(ManifestTest, CreateStartsEmpty) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  EXPECT_EQ(m.manifest_number(), 1u);
  EXPECT_EQ(m.next_file_number(), 2u);
  EXPECT_EQ(Read("CURRENT"), "MANIFEST-000001\n");

  auto r = Recovered();
  EXPECT_EQ(StateOf(*r), DbState{});
  EXPECT_EQ(r->manifest_number(), 1u);
  EXPECT_EQ(r->next_file_number(), 2u);
}

TEST_F(ManifestTest, CreateRefusesExistingDatabase) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  Manifest again(dir_.get());
  EXPECT_TRUE(again.Create().IsInvalidArgument());
}

TEST_F(ManifestTest, CommitThenRecover) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  for (int i = 0; i < 5; ++i) {
    VersionEdit edit = HistoryEdit(m, i);
    ASSERT_OK(m.Commit(&edit));
    EXPECT_EQ(edit.next_file_number, m.next_file_number());
  }
  const DbState state = StateOf(m);
  EXPECT_EQ(state.files.added.size(), 5u);
  EXPECT_EQ(m.current()->files(1).size(), 1u);
  EXPECT_EQ(m.last_sequence(), 500u);

  auto r = Recovered();
  EXPECT_EQ(StateOf(*r), state) << Describe(StateOf(*r)) << " vs " << Describe(state);
  EXPECT_EQ(r->next_file_number(), m.next_file_number());
  EXPECT_EQ(r->manifest_number(), m.manifest_number());

  // Recover doesn't open the MANIFEST for writing.
  VersionEdit edit;
  EXPECT_TRUE(r->Commit(&edit).IsInvalidArgument());
}

TEST_F(ManifestTest, RewriteThenRecover) {
  {
    Manifest m(dir_.get());
    ASSERT_OK(m.Create());
    for (int i = 0; i < 3; ++i) {
      VersionEdit edit = HistoryEdit(m, i);
      ASSERT_OK(m.Commit(&edit));
    }
  }
  auto m = Recovered();
  const uint64_t old_manifest = m->manifest_number();
  const DbState before = StateOf(*m);
  ASSERT_OK(m->Rewrite());
  EXPECT_GT(m->manifest_number(), old_manifest);
  EXPECT_EQ(Read("CURRENT"), ManifestFileName(m->manifest_number()) + "\n");
  EXPECT_EQ(StateOf(*m), before);
  // The old MANIFEST stays until the caller removes it.
  EXPECT_FALSE(Read(ManifestFileName(old_manifest)).empty());

  for (int i = 3; i < 6; ++i) {
    VersionEdit edit = HistoryEdit(*m, i);
    ASSERT_OK(m->Commit(&edit));
  }
  auto r = Recovered();
  EXPECT_EQ(StateOf(*r), StateOf(*m));
  EXPECT_EQ(r->manifest_number(), m->manifest_number());
  EXPECT_EQ(r->next_file_number(), m->next_file_number());
}

TEST_F(ManifestTest, CommitValidation) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  VersionEdit first = HistoryEdit(m, 0);
  ASSERT_OK(m.Commit(&first));
  const DbState state = StateOf(m);

  VersionEdit absent;
  absent.RemoveFile(0, 999);
  EXPECT_TRUE(m.Commit(&absent).IsCorruption());

  VersionEdit unallocated;
  unallocated.AddFile(0, SstInfo{m.next_file_number(), 1, IKey("a"), IKey("b")});
  EXPECT_TRUE(m.Commit(&unallocated).IsInvalidArgument());

  VersionEdit lower_log;
  lower_log.log_number = m.log_number() - 1;
  EXPECT_TRUE(m.Commit(&lower_log).IsInvalidArgument());

  VersionEdit lower_seq;
  lower_seq.last_sequence = m.last_sequence() - 1;
  EXPECT_TRUE(m.Commit(&lower_seq).IsInvalidArgument());

  // None of those changed anything or poisoned the Manifest.
  EXPECT_EQ(StateOf(m), state);
  VersionEdit second = HistoryEdit(m, 1);
  ASSERT_OK(m.Commit(&second));
  EXPECT_EQ(StateOf(*Recovered()), StateOf(m));
}

TEST_F(ManifestTest, FileNumbers) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  const uint64_t a = m.NewFileNumber();
  EXPECT_EQ(m.NewFileNumber(), a + 1);
  m.MarkFileNumberUsed(a);  // already used: no change
  EXPECT_EQ(m.next_file_number(), a + 2);
  m.MarkFileNumberUsed(100);
  EXPECT_EQ(m.NewFileNumber(), 101u);
}

TEST_F(ManifestTest, RecoverMarksFilesOnDiskUsed) {
  {
    Manifest m(dir_.get());
    ASSERT_OK(m.Create());
  }
  Overwrite(SstFileName(50), "x");
  Overwrite(WalFileName(70), "x");
  Overwrite(ManifestFileName(60), "x");
  Overwrite("999999.unrelated", "x");
  auto r = Recovered();
  EXPECT_EQ(r->next_file_number(), 71u);
}

TEST_F(ManifestTest, TornTailIsIgnored) {
  DbState before_last, after_last;
  uint64_t manifest = 0;
  {
    Manifest m(dir_.get());
    ASSERT_OK(m.Create());
    for (int i = 0; i < 4; ++i) {
      VersionEdit edit = HistoryEdit(m, i);
      ASSERT_OK(m.Commit(&edit));
    }
    before_last = StateOf(m);
    VersionEdit edit = HistoryEdit(m, 4);
    ASSERT_OK(m.Commit(&edit));
    after_last = StateOf(m);
    manifest = m.manifest_number();
  }
  const std::string name = ManifestFileName(manifest);
  const std::string full = Read(name);

  // Find where the last record starts by re-reading the records.
  uint64_t last_start = 0;
  {
    WalReader reader(full);
    std::string_view payload;
    uint64_t prev_end = 0;
    while (reader.Next(&payload) == WalReader::Outcome::kRecord) {
      last_start = prev_end;
      prev_end = reader.good_end();
    }
  }
  for (uint64_t cut = last_start + 1; cut < full.size(); ++cut) {
    Overwrite(name, std::string_view(full).substr(0, cut));
    EXPECT_EQ(StateOf(*Recovered()), before_last) << "cut at " << cut;
  }
  for (std::string garbage : {std::string(1, '\0'), std::string(64, '\0'), std::string(3, 'x'),
                              std::string(100, '\xff')}) {
    Overwrite(name, full + garbage);
    EXPECT_EQ(StateOf(*Recovered()), after_last) << test::Printable(garbage);
  }
}

TEST_F(ManifestTest, ChecksummedGarbageIsCorruption) {
  uint64_t manifest = 0;
  uint64_t first_file = 0;
  {
    Manifest m(dir_.get());
    ASSERT_OK(m.Create());
    VersionEdit edit = HistoryEdit(m, 0);
    first_file = edit.added[0].info.number;
    ASSERT_OK(m.Commit(&edit));
    manifest = m.manifest_number();
  }
  const std::string name = ManifestFileName(manifest);
  const std::string good = Read(name);

  AppendRecord(manifest, "\x63garbage");  // unknown tag
  EXPECT_TRUE(RecoverStatus().IsCorruption());

  // Decodes, but doesn't apply.
  Overwrite(name, good);
  VersionEdit absent;
  absent.RemoveFile(3, 12345);
  std::string encoded;
  absent.EncodeTo(&encoded);
  AppendRecord(manifest, encoded);
  EXPECT_TRUE(RecoverStatus().IsCorruption());

  Overwrite(name, good);
  VersionEdit duplicate;
  duplicate.AddFile(0, SstInfo{first_file, 1, IKey("a"), IKey("b")});
  encoded.clear();
  duplicate.EncodeTo(&encoded);
  AppendRecord(manifest, encoded);
  EXPECT_TRUE(RecoverStatus().IsCorruption());
}

TEST_F(ManifestTest, MissingCounterIsCorruption) {
  const std::vector<std::function<void(VersionEdit&)>> drop = {
      [](VersionEdit& e) { e.log_number.reset(); },
      [](VersionEdit& e) { e.next_file_number.reset(); },
      [](VersionEdit& e) { e.last_sequence.reset(); },
  };
  for (const auto& d : drop) {
    VersionEdit edit;
    edit.log_number = 1;
    edit.next_file_number = 5;
    edit.last_sequence = 9;
    d(edit);
    std::string encoded;
    edit.EncodeTo(&encoded);
    Overwrite(ManifestFileName(3), "");
    AppendRecord(3, encoded);
    Overwrite("CURRENT", "MANIFEST-000003\n");
    EXPECT_TRUE(RecoverStatus().IsCorruption());
  }
  // An empty MANIFEST sets nothing.
  Overwrite(ManifestFileName(3), "");
  EXPECT_TRUE(RecoverStatus().IsCorruption());
}

TEST_F(ManifestTest, BadCurrentIsCorruption) {
  {
    Manifest m(dir_.get());
    ASSERT_OK(m.Create());
  }
  ASSERT_OK(RecoverStatus());
  for (std::string contents :
       {std::string(), std::string("\n"), std::string("MANIFEST-000001"),
        std::string("MANIFEST-000001\n\n"), std::string("MANIFEST-000001\r\n"),
        std::string("MANIFEST-1\n"), std::string(" MANIFEST-000001\n"),
        std::string("000001.wal\n"), std::string("000001.sst\n"), std::string("CURRENT\n"),
        std::string("MANIFEST-000099\n"), std::string("MANIFEST-000001\0\n", 17)}) {
    Overwrite("CURRENT", contents);
    EXPECT_TRUE(RecoverStatus().IsCorruption()) << test::Printable(contents);
  }
  Overwrite("CURRENT", "MANIFEST-000001\n");
  EXPECT_OK(RecoverStatus());
}

TEST_F(ManifestTest, StaleCurrentTmpIsReplaced) {
  Overwrite("CURRENT.tmp", "MANIFEST-000777\n");
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  EXPECT_EQ(Read("CURRENT"), "MANIFEST-000001\n");
  std::vector<std::string> names;
  ASSERT_OK(dir_->List(&names));
  EXPECT_EQ(std::count(names.begin(), names.end(), "CURRENT.tmp"), 0);
}

TEST_F(ManifestTest, SyncFailureIsSticky) {
  Manifest m(dir_.get());
  ASSERT_OK(m.Create());
  VersionEdit first = HistoryEdit(m, 0);
  ASSERT_OK(m.Commit(&first));
  const DbState state = StateOf(m);
  const auto version = m.current();

  env_.FailFileSyncs(1);
  VersionEdit failing = HistoryEdit(m, 1);
  const Status s = m.Commit(&failing);
  ASSERT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(StateOf(m), state);
  EXPECT_EQ(m.current(), version);

  // Sticky, though syncs work again.
  VersionEdit next = HistoryEdit(m, 2);
  EXPECT_EQ(m.Commit(&next).ToString(), s.ToString());
  EXPECT_EQ(m.Rewrite().ToString(), s.ToString());
  EXPECT_EQ(StateOf(m), state);

  // Reopening recovers either side of the failed commit.
  auto r = Recovered();
  const DbState got = StateOf(*r);
  EXPECT_TRUE(got == state || got.files.added.size() == 2) << Describe(got);
  ASSERT_OK(r->Rewrite());
}

TEST_F(ManifestTest, SyncFailureInCreateIsSticky) {
  env_.FailFileSyncs(1);
  Manifest m(dir_.get());
  const Status s = m.Create();
  ASSERT_TRUE(s.IsIOError());
  EXPECT_EQ(m.Rewrite().ToString(), s.ToString());
  VersionEdit edit;
  EXPECT_FALSE(m.Commit(&edit).ok());

  Manifest again(dir_.get());
  bool exists = true;
  ASSERT_OK(again.Recover(&exists));
  EXPECT_FALSE(exists);
  ASSERT_OK(again.Create());  // the leftover MANIFEST's number isn't reused
  EXPECT_EQ(again.manifest_number(), 2u);
}

TEST(ManifestPosixTest, RealDisk) {
  test::TempDir tmp;
  std::unique_ptr<Directory> dir;
  ASSERT_OK(Env::Posix()->OpenDirectory(tmp.path(), &dir));
  DbState state;
  {
    Manifest m(dir.get());
    ASSERT_OK(m.Create());
    for (int i = 0; i < 3; ++i) {
      VersionEdit edit = HistoryEdit(m, i);
      ASSERT_OK(m.Commit(&edit));
    }
    state = StateOf(m);
  }
  for (int round = 0; round < 2; ++round) {
    Manifest m(dir.get());
    bool exists = false;
    ASSERT_OK(m.Recover(&exists));
    ASSERT_TRUE(exists);
    EXPECT_EQ(StateOf(m), state);
    ASSERT_OK(m.Rewrite());
    VersionEdit edit = HistoryEdit(m, 3 + round);
    ASSERT_OK(m.Commit(&edit));
    state = StateOf(m);
  }
  Manifest m(dir.get());
  bool exists = false;
  ASSERT_OK(m.Recover(&exists));
  EXPECT_EQ(StateOf(m), state);
}

// ---------------------------------------------------------------------------
// Power loss before every mutating step of Create, Commit and Rewrite.

enum class Op { kCreate, kCommit, kRewrite };

struct Outcome {
  std::optional<DbState> before;  // nullopt: no database yet
  DbState after;
};

// Brings the directory to the state just before op, then runs op. Returns
// op's status. Deterministic, so a run without power loss tells what the
// state after a successful op is.
Status RunOp(Op op, Directory* dir, std::unique_ptr<Manifest>* holder, Outcome* outcome,
             const std::function<void()>& arm) {
  auto& m = *holder;
  m = std::make_unique<Manifest>(dir);
  if (op == Op::kCreate) {
    outcome->before.reset();
    arm();
    return m->Create();
  }
  EXPECT_OK(m->Create());
  for (int i = 0; i < 3; ++i) {
    VersionEdit edit = HistoryEdit(*m, i);
    EXPECT_OK(m->Commit(&edit));
  }
  if (op == Op::kRewrite) {
    m = std::make_unique<Manifest>(dir);
    bool exists = false;
    EXPECT_OK(m->Recover(&exists));
  }
  outcome->before = StateOf(*m);
  if (op == Op::kCommit) {
    VersionEdit edit = HistoryEdit(*m, 3);
    arm();
    return m->Commit(&edit);
  }
  arm();
  return m->Rewrite();
}

void CheckPowerLoss(Op op, PowerLossMode mode, uint64_t seed) {
  // The state a successful op produces.
  DbState expected_after;
  {
    FaultInjectionEnv env;
    ASSERT_OK(env.CreateDirectory("/db"));
    std::unique_ptr<Directory> dir;
    ASSERT_OK(env.OpenDirectory("/db", &dir));
    std::unique_ptr<Manifest> m;
    Outcome outcome;
    ASSERT_OK(RunOp(op, dir.get(), &m, &outcome, [] {}));
    expected_after = StateOf(*m);
  }

  uint64_t n = 1;
  for (;; ++n) {
    SCOPED_TRACE("power loss before mutation " + std::to_string(n));
    FaultInjectionEnv env;
    ASSERT_OK(env.CreateDirectory("/db"));
    std::unique_ptr<Directory> dir;
    ASSERT_OK(env.OpenDirectory("/db", &dir));
    std::unique_ptr<Manifest> m;
    Outcome outcome;
    uint64_t losses = 0;
    const Status s = RunOp(op, dir.get(), &m, &outcome, [&] {
      losses = env.power_losses();
      env.SchedulePowerLoss(n, mode, seed);
    });
    const bool crashed = env.power_losses() > losses;
    if (crashed) {
      ASSERT_FALSE(s.ok());
    } else {
      // The op finished before the nth mutation; an OK op must survive.
      ASSERT_OK(s);
      env.SimulatePowerLoss(mode, seed);
    }
    m.reset();

    ASSERT_OK(env.OpenDirectory("/db", &dir));
    auto r = std::make_unique<Manifest>(dir.get());
    bool exists = false;
    ASSERT_OK(r->Recover(&exists));
    if (!exists) {
      ASSERT_TRUE(crashed);
      ASSERT_EQ(op, Op::kCreate);
      r = std::make_unique<Manifest>(dir.get());
      ASSERT_OK(r->Create());
    } else {
      const DbState got = StateOf(*r);
      if (crashed) {
        EXPECT_TRUE((outcome.before && got == *outcome.before) || got == expected_after)
            << Describe(got);
      } else {
        EXPECT_EQ(got, expected_after) << Describe(got);
      }
      ASSERT_OK(r->Rewrite());
    }

    // The recovered database keeps working: no file number collides with a
    // leftover from the crash.
    const DbState recovered = StateOf(*r);
    VersionEdit edit = HistoryEdit(*r, 5);
    ASSERT_OK(r->Commit(&edit));
    const DbState committed = StateOf(*r);
    EXPECT_NE(committed, recovered);
    r.reset();
    auto again = std::make_unique<Manifest>(dir.get());
    ASSERT_OK(again->Recover(&exists));
    ASSERT_TRUE(exists);
    EXPECT_EQ(StateOf(*again), committed);

    if (!crashed) break;
    ASSERT_LT(n, 100u) << "op never completed";
  }
  // Commit is an append and a sync; Create and Rewrite take ten steps.
  EXPECT_GE(n, op == Op::kCommit ? 3u : 11u);
}

class ManifestPowerLossTest : public ::testing::TestWithParam<Op> {};

TEST_P(ManifestPowerLossTest, DropUnsynced) {
  CheckPowerLoss(GetParam(), PowerLossMode::kDropUnsynced, 0);
}

TEST_P(ManifestPowerLossTest, TornWrites) {
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    CheckPowerLoss(GetParam(), PowerLossMode::kTornWrites, seed);
    if (HasFatalFailure()) return;
  }
}

INSTANTIATE_TEST_SUITE_P(Ops, ManifestPowerLossTest,
                         ::testing::Values(Op::kCreate, Op::kCommit, Op::kRewrite),
                         [](const auto& info) {
                           switch (info.param) {
                             case Op::kCreate: return "Create";
                             case Op::kCommit: return "Commit";
                             case Op::kRewrite: return "Rewrite";
                           }
                           return "Unknown";
                         });

}  // namespace
}  // namespace lsmkv
