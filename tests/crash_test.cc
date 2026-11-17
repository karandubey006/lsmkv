// Crash tests. Each cycle runs a random workload, crashes it, reopens the
// database and checks one property:
//
//   the recovered contents equal the state after the first k operations of
//   the workload, for some k between "everything the engine promised to keep"
//   and "everything that was attempted".
//
// That single check rules out lost acknowledged writes, prefix violations,
// torn batches, resurrected deletes and phantom values.
//
// Power-loss mode runs on the fault-injection Env. Power fails just before a
// random mutating file operation (inside a write, a memtable rotation or a
// recovery) and unsynced bytes are dropped or mangled. Every write
// acknowledged with sync=true must survive.
//
// Kill mode forks a child that writes to a real directory and SIGKILLs it at a
// random point. Every acknowledged write must survive, synced or not.
//
// Rerun a failure with the seed it prints: LSMKV_CRASH_SEED=<seed>.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <vector>

#include "env/fault_injection_env.h"
#include "lsmkv/db.h"
#include "test_util.h"

namespace lsmkv {
namespace {

using State = std::map<std::string, std::string>;
using Mode = FaultInjectionEnv::PowerLossMode;

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

constexpr int kKeys = 40;

struct Op {
  std::vector<std::pair<std::string, std::optional<std::string>>> mutations;  // nullopt = delete
  bool sync = false;
};

// Values name the op that wrote them, so a stale or phantom value never
// compares equal to the right one.
std::vector<Op> GenerateOps(std::mt19937_64& rng, size_t n, uint64_t first_id, int sync_one_in) {
  std::vector<Op> ops(n);
  for (size_t i = 0; i < n; ++i) {
    Op& op = ops[i];
    const uint64_t roll = rng() % 100;
    const size_t mutations = roll < 85 ? 1 : 2 + rng() % 5;
    for (size_t j = 0; j < mutations; ++j) {
      std::string key = "k" + std::to_string(rng() % kKeys);
      if (roll >= 70 && roll < 85) {
        op.mutations.emplace_back(std::move(key), std::nullopt);
        continue;
      }
      std::string value = "op" + std::to_string(first_id + i) + "." + std::to_string(j) + ":";
      value.append(rng() % 10 == 0 ? rng() % 5000 : rng() % 200, 'v');  // some span pages
      if (rng() % 4 == 0 && j > 0) {
        op.mutations.emplace_back(std::move(key), std::nullopt);
      } else {
        op.mutations.emplace_back(std::move(key), std::move(value));
      }
    }
    op.sync = rng() % sync_one_in == 0;
  }
  return ops;
}

WriteBatch ToBatch(const Op& op) {
  WriteBatch b;
  for (const auto& [key, value] : op.mutations) {
    if (value) b.Put(key, *value);
    else       b.Delete(key);
  }
  return b;
}

void Apply(const Op& op, State& state) {
  for (const auto& [key, value] : op.mutations) {
    if (value) state[key] = *value;
    else       state.erase(key);
  }
}

// The database's contents, via an iterator, cross-checked key by key with Get.
State ReadAll(DB& db) {
  State state;
  auto it = db.NewIterator(ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) state.emplace(it->key(), it->value());
  EXPECT_OK(it->status());
  for (int i = 0; i < kKeys; ++i) {
    const std::string key = "k" + std::to_string(i);
    std::string value;
    Status s = db.Get(ReadOptions(), key, &value);
    auto found = state.find(key);
    if (found == state.end()) {
      EXPECT_TRUE(s.IsNotFound()) << key << ": " << s.ToString();
    } else {
      EXPECT_OK(s);
      EXPECT_EQ(value, found->second) << key;
    }
  }
  return state;
}

// Finds k in [min_k, max_k] such that applying ops[0, k) to base gives got.
std::optional<size_t> MatchingPrefix(const State& base, const std::vector<Op>& ops, size_t min_k,
                                     size_t max_k, const State& got) {
  State state = base;
  for (size_t k = 0; k <= max_k; ++k) {
    if (k >= min_k && state == got) return k;
    if (k < ops.size()) Apply(ops[k], state);
  }
  return std::nullopt;
}

std::string Describe(const State& s) {
  std::ostringstream out;
  for (const auto& [k, v] : s) out << " " << k << "=" << v.substr(0, v.find(':'));
  return out.str();
}

// ---- power-loss mode --------------------------------------------------------

void RunPowerLossSeed(uint64_t seed) {
  SCOPED_TRACE("LSMKV_CRASH_SEED=" + std::to_string(seed));
  std::mt19937_64 rng(seed);
  FaultInjectionEnv env;
  Options options;
  options.create_if_missing = true;
  options.env = &env;
  options.memtable_size = 4096;  // rotate every few writes
  State base;                    // what the previous cycle recovered
  uint64_t next_op_id = 0;

  for (int cycle = 0; cycle < 4; ++cycle) {
    SCOPED_TRACE("cycle " + std::to_string(cycle));
    const auto mode = rng() % 2 ? Mode::kTornWrites : Mode::kDropUnsynced;
    std::unique_ptr<DB> db;

    // Sometimes the power fails during recovery itself.
    if (cycle > 0 && rng() % 4 == 0) {
      env.SchedulePowerLoss(1 + rng() % 3, mode, rng());
      if (DB::Open(options, "/db", &db).ok()) {
        db.reset();
        env.SimulatePowerLoss(mode, rng());
      }
      ASSERT_OK(DB::Open(options, "/db", &db));
      ASSERT_EQ(ReadAll(*db), base) << "a crash during recovery changed the data";
      db.reset();
    }

    ASSERT_OK(DB::Open(options, "/db", &db));
    // Rarer syncs leave a longer unsynced tail for the power loss to tear.
    const std::vector<Op> ops = GenerateOps(rng, 40, next_op_id, static_cast<int>(2 + rng() % 9));
    next_op_id += ops.size();

    const uint64_t losses = env.power_losses();
    env.SchedulePowerLoss(1 + rng() % 120, mode, rng());
    size_t acked = 0, durable = 0;  // ops acknowledged; ops through the last synced ack
    bool in_flight = false;
    for (size_t i = 0; i < ops.size(); ++i) {
      WriteOptions wo;
      wo.sync = ops[i].sync;
      Status s = db->Write(wo, ToBatch(ops[i]));
      if (!s.ok()) {
        ASSERT_TRUE(s.IsIOError()) << s.ToString();
        ASSERT_GT(env.power_losses(), losses) << "write failed without a power loss";
        in_flight = true;
        break;
      }
      acked = i + 1;
      if (ops[i].sync) durable = acked;
    }
    if (env.power_losses() == losses) env.SimulatePowerLoss(mode, rng());
    db.reset();

    ASSERT_OK(DB::Open(options, "/db", &db));
    const State got = ReadAll(*db);
    const size_t max_k = acked + (in_flight ? 1 : 0);
    const auto k = MatchingPrefix(base, ops, durable, max_k, got);
    ASSERT_TRUE(k.has_value()) << "recovered state is not a prefix in [" << durable << ", " << max_k
                               << "] of the history\n  recovered:" << Describe(got);
    base = got;
  }
}

TEST(CrashTest, PowerLossKeepsEverySyncedWriteAndAPrefix) {
  if (const char* one = std::getenv("LSMKV_CRASH_SEED")) {
    RunPowerLossSeed(std::strtoull(one, nullptr, 10));
    return;
  }
  const uint64_t seeds = EnvOr("LSMKV_CRASH_SEEDS", 300);
  for (uint64_t seed = 0; seed < seeds && !::testing::Test::HasFailure(); ++seed) {
    RunPowerLossSeed(seed);
  }
}

// ---- kill mode --------------------------------------------------------------

bool WriteAll(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Returns false at end of file.
bool ReadAll4(int fd, uint32_t* out) {
  char* p = reinterpret_cast<char*>(out);
  size_t got = 0;
  while (got < 4) {
    ssize_t r = ::read(fd, p + got, 4 - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

// Runs in the forked child. Reports each acknowledged op number over the pipe
// *after* Write returns OK, so the parent's count never runs ahead of the acks.
[[noreturn]] void RunChild(const std::string& path, const std::vector<Op>& ops, int ack_fd) {
  Options options;
  options.create_if_missing = true;
  options.memtable_size = 16 << 10;
  std::unique_ptr<DB> db;
  if (!DB::Open(options, path, &db).ok()) ::_exit(2);
  for (uint32_t i = 0; i < ops.size(); ++i) {
    WriteOptions wo;
    wo.sync = ops[i].sync;
    if (!db->Write(wo, ToBatch(ops[i])).ok()) ::_exit(3);
    const uint32_t acked = i + 1;
    if (!WriteAll(ack_fd, &acked, sizeof(acked))) ::_exit(4);
  }
  ::_exit(0);
}

TEST(CrashTest, KillKeepsEveryAcknowledgedWrite) {
  const uint64_t seed = EnvOr("LSMKV_CRASH_SEED", 1);
  SCOPED_TRACE("LSMKV_CRASH_SEED=" + std::to_string(seed));
  std::mt19937_64 rng(seed);
  test::TempDir dir;
  const std::string path = dir.path() + "/db";
  State base;
  uint64_t next_op_id = 0, in_flight_present = 0, in_flight_absent = 0;

  const uint64_t cycles = EnvOr("LSMKV_KILL_CYCLES", 20);
  for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
    SCOPED_TRACE("cycle " + std::to_string(cycle));
    const std::vector<Op> ops = GenerateOps(rng, 2000, next_op_id, /*sync_one_in=*/50);
    next_op_id += ops.size();
    const uint32_t kill_after = static_cast<uint32_t>(1 + rng() % ops.size());

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(fds[0]);
      RunChild(path, ops, fds[1]);
    }
    ::close(fds[1]);

    // Kill as soon as the target ack arrives; the child is mid-write by then.
    uint32_t acked = 0, ack = 0;
    bool killed = false;
    while (ReadAll4(fds[0], &ack)) {
      acked = ack;
      if (!killed && acked >= kill_after) {
        ::kill(pid, SIGKILL);
        killed = true;
      }
    }
    ::close(fds[0]);  // EOF: the child is dead, and every ack it sent was read
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    if (!WIFSIGNALED(status)) {
      ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child failed: " << status;
    }

    std::unique_ptr<DB> db;
    Options options;
    ASSERT_OK(DB::Open(options, path, &db));
    const State got = ReadAll(*db);
    const size_t max_k = std::min<size_t>(acked + 1, ops.size());
    const auto k = MatchingPrefix(base, ops, acked, max_k, got);
    ASSERT_TRUE(k.has_value()) << "recovered state is not the first " << acked << " or " << max_k
                               << " ops\n  recovered:" << Describe(got);
    if (*k > acked) ++in_flight_present;
    else if (acked < ops.size()) ++in_flight_absent;
    base = got;
  }
  std::cout << "[kill] cycles=" << cycles << " in_flight_present=" << in_flight_present
            << " in_flight_absent=" << in_flight_absent << "\n";
}

}  // namespace
}  // namespace lsmkv
