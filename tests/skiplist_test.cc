#include "memtable/skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "memtable/arena.h"

namespace lsmkv {
namespace {

struct U64Comparator {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
using U64List = SkipList<uint64_t, U64Comparator>;

TEST(SkipListTest, Empty) {
  Arena arena;
  U64List list(U64Comparator{}, &arena);
  EXPECT_FALSE(list.Contains(10));

  U64List::Iterator it(&list);
  EXPECT_FALSE(it.Valid());
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, MatchesStdSet) {
  constexpr int kN = 2000;
  constexpr uint64_t kRange = 5000;
  std::mt19937_64 rng(1000);
  std::set<uint64_t> model;
  Arena arena;
  U64List list(U64Comparator{}, &arena);
  for (int i = 0; i < kN; ++i) {
    const uint64_t key = rng() % kRange;
    if (model.insert(key).second) list.Insert(key);
  }

  for (uint64_t k = 0; k < kRange; ++k) {
    ASSERT_EQ(list.Contains(k), model.count(k) == 1) << k;
  }

  // Seek lands on the first key >= target, like std::set::lower_bound.
  for (uint64_t target = 0; target < kRange + 10; ++target) {
    U64List::Iterator it(&list);
    it.Seek(target);
    auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      ASSERT_FALSE(it.Valid()) << target;
    } else {
      ASSERT_TRUE(it.Valid()) << target;
      ASSERT_EQ(it.key(), *expected) << target;
    }
  }

  // A full scan returns every key in order.
  U64List::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t expected : model) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), expected);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
}

// ---------------------------------------------------------------------------
// One writer, four lock-free readers.
//
// The writer inserts a shuffled set of keys. After each insert it bumps
// `published`, with release order. Each key carries a checksum in its low bits,
// so a reader that sees a node whose key isn't fully written fails loudly.
//
// Readers check, without any lock:
//   1. Every key they see is well-formed (its checksum matches).
//   2. A scan returns keys in strictly increasing order.
//   3. A scan sees every key that was published before the scan began.
//   4. Seek(t) lands on a key >= t and skips no key the scan saw.
//
// Under TSan, this also catches missing release/acquire ordering.
// ---------------------------------------------------------------------------

// Key layout: id in the high 32 bits, a hash of the id in the low 32 bits.
// Keys sort by id, and every id appears at most once.
uint64_t MakeKey(uint32_t id) {
  const uint32_t check = static_cast<uint32_t>((uint64_t{id} * 0x9E3779B97F4A7C15ULL) >> 32) | 1u;
  return (uint64_t{id} << 32) | check;
}
bool IsWellFormed(uint64_t key) { return MakeKey(static_cast<uint32_t>(key >> 32)) == key; }

TEST(SkipListTest, ConcurrentReadersDuringInserts) {
  constexpr uint32_t kKeys = 20000;
  constexpr int kReaders = 4;

  std::vector<uint32_t> order(kKeys);
  std::iota(order.begin(), order.end(), 0u);
  std::shuffle(order.begin(), order.end(), std::mt19937(42));

  Arena arena;
  U64List list(U64Comparator{}, &arena);

  // published = n means order[0..n) are fully inserted. It's only used by the
  // test to know what a reader MUST see; the skip list doesn't rely on it.
  std::atomic<uint32_t> published{0};
  std::atomic<bool> failed{false};

  auto reader = [&](uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<bool> seen(kKeys);
    while (!failed.load(std::memory_order_relaxed)) {
      const uint32_t n = published.load(std::memory_order_acquire);

      // Full scan: well-formed, strictly increasing, and nothing published is missing.
      std::fill(seen.begin(), seen.end(), false);
      U64List::Iterator it(&list);
      uint64_t prev = 0;
      bool first = true;
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        const uint64_t k = it.key();
        if (!IsWellFormed(k) || (!first && k <= prev)) {
          ADD_FAILURE() << "bad key or order: " << k << " after " << prev;
          failed = true;
          return;
        }
        seen[k >> 32] = true;
        prev = k;
        first = false;
      }
      for (uint32_t i = 0; i < n; ++i) {
        if (!seen[order[i]]) {
          ADD_FAILURE() << "published key " << order[i] << " missing from a scan";
          failed = true;
          return;
        }
      }

      // Random seeks. Every key seen by the scan is still in the list (nothing
      // is ever removed), so Seek(t) must land on the first one >= t or earlier,
      // never past it.
      for (int s = 0; s < 50; ++s) {
        const uint32_t id = static_cast<uint32_t>(rng() % kKeys);
        const uint64_t target = MakeKey(id) - (rng() % 2);  // on a key, or just before it
        it.Seek(target);
        if (it.Valid() && (!IsWellFormed(it.key()) || it.key() < target)) {
          ADD_FAILURE() << "Seek(" << target << ") landed on " << it.key();
          failed = true;
          return;
        }
        const uint32_t landed = it.Valid() ? static_cast<uint32_t>(it.key() >> 32) : kKeys;
        for (uint32_t j = id; j < landed; ++j) {
          if (seen[j]) {
            ADD_FAILURE() << "Seek(" << target << ") skipped key " << j;
            failed = true;
            return;
          }
        }
      }
      if (n == kKeys) return;  // everything was published before this pass: done
    }
  };

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) readers.emplace_back(reader, 100 + r);

  for (uint32_t i = 0; i < kKeys; ++i) {
    list.Insert(MakeKey(order[i]));
    published.store(i + 1, std::memory_order_release);
  }
  for (auto& t : readers) t.join();
  ASSERT_FALSE(failed.load());

  // Afterwards, everything is there.
  for (uint32_t id = 0; id < kKeys; ++id) ASSERT_TRUE(list.Contains(MakeKey(id))) << id;
}

}  // namespace
}  // namespace lsmkv
