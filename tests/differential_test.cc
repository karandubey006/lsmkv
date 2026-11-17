// Differential tester: runs a seeded random sequence of operations against
// lsmkv and against a std::map model, and fails on the first difference.
//
// It only uses the public API, so it keeps working unchanged as the engine
// underneath grows from a std::map stand-in into a real LSM-tree. Later phases
// add operations (reopen, crash, force flush/compaction) to the mix.
//
// Every failure prints the seed and the last operations; rerun exactly with
//   LSMKV_DIFF_SEED=<seed> LSMKV_DIFF_OPS=<n> ctest --preset asan -R Differential

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>

#include "env/fault_injection_env.h"
#include "lsmkv/db.h"
#include "test_util.h"

namespace lsmkv {
namespace {

using Model = std::map<std::string, std::string>;
using test::Printable;

// The tester is single-threaded, so ThreadSanitizer has nothing to find in it
// beyond what the threaded tests cover, and it slows the run about 6x.
#if defined(__SANITIZE_THREAD__)
constexpr uint64_t kDefaultOps = 10'000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr uint64_t kDefaultOps = 10'000;
#else
constexpr uint64_t kDefaultOps = 100'000;
#endif
#else
constexpr uint64_t kDefaultOps = 100'000;
#endif

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

class DifferentialTester {
 public:
  // Key space: mostly a dense range of readable keys so operations keep hitting
  // existing keys, plus a pool of awkward ones (empty, binary, embedded NUL,
  // bytes >= 0x80, prefixes of other keys).
  static constexpr int kDenseKeys = 2000;
  static constexpr int kAwkwardKeys = 200;

  // reopen closes the DB and opens it again, as a clean restart would.
  DifferentialTester(std::unique_ptr<DB>& db, std::function<Status()> reopen, uint64_t seed)
      : db_(db), reopen_(std::move(reopen)), seed_(seed), rng_(seed) {
    awkward_keys_.push_back("");
    for (int i = 0; i < 20; ++i) awkward_keys_.push_back("user" + std::to_string(i));
    while (awkward_keys_.size() < kAwkwardKeys) {
      awkward_keys_.push_back(RandomBytes(Uniform(1, 16)));
    }
  }

  void Run(uint64_t num_ops) {
    num_ops_ = num_ops;
    for (op_index_ = 0; op_index_ < num_ops; ++op_index_) {
      if (OneIn(5000)) {
        DoReopen();
        if (::testing::Test::HasFailure()) return;
        continue;
      }
      const uint64_t roll = Uniform(0, 999);
      if (roll < 300)      DoPut();
      else if (roll < 420) DoDelete();
      else if (roll < 500) DoBatch();
      else if (roll < 835) DoGet();
      else if (roll < 985) DoScan();
      else if (roll < 995) DoSnapshot();
      else                 DoFullScan();
      if (::testing::Test::HasFailure()) return;
    }
    Record("final full scan");
    ExpectFullScanMatches(model_);

    std::cout << "[differential] seed=" << seed_ << " ops=" << num_ops
              << " puts=" << stats_.puts << " deletes=" << stats_.deletes
              << " batches=" << stats_.batches << " gets=" << stats_.gets
              << " (hits=" << stats_.get_hits << ") scans=" << stats_.scans
              << " snapshots=" << stats_.snapshots << " reopens=" << stats_.reopens
              << " final_keys=" << model_.size()
              << "\n";
  }

 private:
  // ---- random generation ---------------------------------------------------

  uint64_t Uniform(uint64_t lo, uint64_t hi) {  // inclusive
    return std::uniform_int_distribution<uint64_t>(lo, hi)(rng_);
  }
  bool OneIn(uint64_t n) { return Uniform(0, n - 1) == 0; }

  std::string RandomBytes(size_t len) {
    std::string s(len, '\0');
    for (char& c : s) c = static_cast<char>(Uniform(0, 255));
    return s;
  }

  std::string RandomKey() {
    if (OneIn(10)) return awkward_keys_[Uniform(0, awkward_keys_.size() - 1)];
    char buf[16];
    std::snprintf(buf, sizeof(buf), "user%06d", static_cast<int>(Uniform(0, kDenseKeys - 1)));
    return buf;
  }

  std::string RandomValue() {
    if (OneIn(20)) return "";
    if (OneIn(100)) return RandomBytes(Uniform(1000, 8192));  // large value
    return RandomBytes(Uniform(1, 100));
  }

  WriteOptions RandomWriteOptions() {
    WriteOptions o;
    o.sync = OneIn(2);
    return o;
  }

  // ---- operations ----------------------------------------------------------

  void DoPut() {
    std::string key = RandomKey(), value = RandomValue();
    Record("Put(" + Printable(key) + ", " + Printable(value) + ")");
    Status s = db_->Put(RandomWriteOptions(), key, value);
    ASSERT_TRUE(s.ok()) << s.ToString() << Context();
    model_[key] = value;
    ++stats_.puts;
  }

  void DoDelete() {
    std::string key = RandomKey();
    Record("Delete(" + Printable(key) + ")");
    Status s = db_->Delete(RandomWriteOptions(), key);
    ASSERT_TRUE(s.ok()) << s.ToString() << Context();
    model_.erase(key);
    ++stats_.deletes;
  }

  void DoBatch() {
    WriteBatch batch;
    std::vector<std::pair<std::string, std::optional<std::string>>> ops;  // nullopt = delete
    const uint64_t n = Uniform(1, 16);
    std::ostringstream desc;
    desc << "Batch[";
    for (uint64_t i = 0; i < n; ++i) {
      // Sometimes reuse a key already in this batch, to test last-op-wins.
      std::string key = (!ops.empty() && OneIn(5)) ? ops[Uniform(0, ops.size() - 1)].first
                                                   : RandomKey();
      if (Uniform(0, 9) < 7) {
        std::string value = RandomValue();
        batch.Put(key, value);
        desc << " Put(" << Printable(key, 16) << ")";
        ops.emplace_back(std::move(key), std::move(value));
      } else {
        batch.Delete(key);
        desc << " Delete(" << Printable(key, 16) << ")";
        ops.emplace_back(std::move(key), std::nullopt);
      }
    }
    desc << " ]";
    Record(desc.str());
    Status s = db_->Write(RandomWriteOptions(), batch);
    ASSERT_TRUE(s.ok()) << s.ToString() << Context();
    for (auto& [key, value] : ops) {
      if (value) model_[key] = *value;
      else       model_.erase(key);
    }
    ++stats_.batches;
  }

  void DoGet() {
    std::string key = RandomKey();
    Record("Get(" + Printable(key) + ")");
    std::string value;
    Status s = db_->Get(ReadOptions(), key, &value);
    auto it = model_.find(key);
    if (it == model_.end()) {
      ASSERT_TRUE(s.IsNotFound()) << "expected NotFound for " << Printable(key) << ", got "
                                  << s.ToString() << " value=" << Printable(value) << Context();
    } else {
      ASSERT_TRUE(s.ok()) << "expected " << Printable(it->second) << " for " << Printable(key)
                          << ", got " << s.ToString() << Context();
      ASSERT_EQ(value, it->second) << "wrong value for " << Printable(key) << ": got "
                                   << Printable(value) << ", expected " << Printable(it->second)
                                   << Context();
      ++stats_.get_hits;
    }
    ++stats_.gets;
  }

  void DoScan() {
    const bool from_start = OneIn(10);
    std::string start = from_start ? "" : RandomKey();
    const size_t limit = Uniform(1, 50);
    Record((from_start ? std::string("Scan(first") : "Scan(" + Printable(start)) + ", limit=" +
           std::to_string(limit) + ")");
    auto it = db_->NewIterator(ReadOptions());
    if (from_start) it->SeekToFirst();
    else            it->Seek(start);
    ExpectRangeMatches(it.get(), model_, model_.lower_bound(start), limit);
    ++stats_.scans;
  }

  // An iterator must keep seeing the database as of its creation while
  // further writes land.
  void DoSnapshot() {
    Record("Snapshot: NewIterator");
    auto it = db_->NewIterator(ReadOptions());
    const Model snapshot = model_;
    const uint64_t writes = Uniform(1, 20);
    for (uint64_t i = 0; i < writes; ++i) {
      switch (Uniform(0, 2)) {
        case 0: DoPut(); break;
        case 1: DoDelete(); break;
        case 2: DoBatch(); break;
      }
      if (::testing::Test::HasFailure()) return;
    }
    Record("Snapshot: scan old iterator after " + std::to_string(writes) + " writes");
    it->SeekToFirst();
    ExpectRangeMatches(it.get(), snapshot, snapshot.begin(), snapshot.size() + 1);
    ++stats_.snapshots;
  }

  void DoFullScan() {
    Record("FullScan");
    ExpectFullScanMatches(model_);
  }

  // Every write was acknowledged and the process didn't crash, so recovery
  // must bring all of them back.
  void DoReopen() {
    Record("Reopen");
    Status s = reopen_();
    ASSERT_TRUE(s.ok()) << s.ToString() << Context();
    ExpectFullScanMatches(model_);
    ++stats_.reopens;
  }

  // ---- checking ------------------------------------------------------------

  void ExpectFullScanMatches(const Model& model) {
    auto it = db_->NewIterator(ReadOptions());
    it->SeekToFirst();
    ExpectRangeMatches(it.get(), model, model.begin(), model.size() + 1);
  }

  // Compares up to `limit` entries of `it` (already positioned) with the model
  // starting at `mit`. If the model runs out first, the iterator must too.
  void ExpectRangeMatches(Iterator* it, const Model& model, Model::const_iterator mit,
                          size_t limit) {
    for (size_t i = 0; i < limit; ++i, ++mit) {
      if (mit == model.end()) {
        ASSERT_FALSE(it->Valid()) << "engine has extra key " << Printable(it->key())
                                  << " at scan position " << i << Context();
        break;
      }
      ASSERT_TRUE(it->Valid()) << "engine ended early; expected key " << Printable(mit->first)
                               << " at scan position " << i << " (status "
                               << it->status().ToString() << ")" << Context();
      ASSERT_EQ(it->key(), mit->first) << "key mismatch at scan position " << i << ": got "
                                       << Printable(it->key()) << ", expected "
                                       << Printable(mit->first) << Context();
      ASSERT_EQ(it->value(), mit->second) << "value mismatch for " << Printable(mit->first)
                                          << Context();
      it->Next();
    }
    Status s = it->status();
    ASSERT_TRUE(s.ok()) << s.ToString() << Context();
  }

  // ---- failure context -----------------------------------------------------

  void Record(std::string desc) {
    recent_.push_back("#" + std::to_string(op_index_) + " " + std::move(desc));
    if (recent_.size() > 25) recent_.pop_front();
  }

  std::string Context() const {
    std::ostringstream out;
    out << "\n  seed=" << seed_ << ", failed during op #" << op_index_ << " of " << num_ops_
        << "\n  replay: LSMKV_DIFF_SEED=" << seed_ << " LSMKV_DIFF_OPS=" << num_ops_
        << "\n  most recent operations:";
    for (const std::string& op : recent_) out << "\n    " << op;
    return out.str();
  }

  struct Stats {
    uint64_t puts = 0, deletes = 0, batches = 0, gets = 0, get_hits = 0, scans = 0,
             snapshots = 0, reopens = 0;
  };

  std::unique_ptr<DB>& db_;
  const std::function<Status()> reopen_;
  const uint64_t seed_;
  std::mt19937_64 rng_;
  std::vector<std::string> awkward_keys_;
  Model model_;
  Stats stats_;
  uint64_t op_index_ = 0;
  uint64_t num_ops_ = 0;
  std::deque<std::string> recent_;
};

class DifferentialTest : public ::testing::TestWithParam<uint64_t> {};

// Runs on the in-memory Env: this checks the engine's logic, and real fsyncs
// would make it take minutes. The crash tests cover the real filesystem.
TEST_P(DifferentialTest, MatchesStdMapModel) {
  FaultInjectionEnv env;
  Options options;
  options.create_if_missing = true;
  options.env = &env;
  options.memtable_size = 256 << 10;  // small, so memtables rotate often
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, "/db", &db));
  auto reopen = [&]() {
    db.reset();
    return DB::Open(options, "/db", &db);
  };

  const uint64_t seed = EnvOr("LSMKV_DIFF_SEED", GetParam());
  const uint64_t ops = EnvOr("LSMKV_DIFF_OPS", kDefaultOps);
  DifferentialTester(db, reopen, seed).Run(ops);
}

INSTANTIATE_TEST_SUITE_P(Seeds, DifferentialTest, ::testing::Values(1, 2, 3));

}  // namespace
}  // namespace lsmkv
