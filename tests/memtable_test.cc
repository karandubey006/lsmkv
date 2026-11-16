#include "memtable/memtable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace lsmkv {
namespace {

using test::Printable;

// The result of a memtable Get, as one comparable value.
struct Lookup {
  MemTable::GetResult result = MemTable::GetResult::kAbsent;
  std::string value;
  bool operator==(const Lookup&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Lookup& r) {
  switch (r.result) {
    case MemTable::GetResult::kAbsent: return os << "<absent>";
    case MemTable::GetResult::kDeleted: return os << "<tombstone>";
    case MemTable::GetResult::kFound: return os << Printable(r.value);
  }
  return os;
}

Lookup Get(const MemTable& mem, std::string_view key, SequenceNumber snapshot) {
  Lookup r;
  std::string value = "garbage";
  r.result = mem.Get(key, snapshot, &value);
  if (r.result == MemTable::GetResult::kFound) r.value = value;
  return r;
}

Lookup Found(std::string v) { return {MemTable::GetResult::kFound, std::move(v)}; }
const Lookup kDeleted{MemTable::GetResult::kDeleted, ""};
const Lookup kAbsent{};

TEST(MemTableTest, AppleExample) {
  MemTable mem;
  mem.Add(2, ValueType::kValue, "apple", "green");
  mem.Add(5, ValueType::kDeletion, "apple", "");
  mem.Add(6, ValueType::kValue, "banana", "yellow");
  mem.Add(7, ValueType::kValue, "apple", "red");

  EXPECT_EQ(Get(mem, "apple", kMaxSequenceNumber), Found("red"));
  EXPECT_EQ(Get(mem, "apple", 7), Found("red"));
  EXPECT_EQ(Get(mem, "apple", 6), kDeleted);
  EXPECT_EQ(Get(mem, "apple", 5), kDeleted);
  EXPECT_EQ(Get(mem, "apple", 4), Found("green"));
  EXPECT_EQ(Get(mem, "apple", 2), Found("green"));
  EXPECT_EQ(Get(mem, "apple", 1), kAbsent);
  EXPECT_EQ(Get(mem, "banana", 6), Found("yellow"));
  EXPECT_EQ(Get(mem, "banana", 5), kAbsent);
  EXPECT_EQ(Get(mem, "appl", kMaxSequenceNumber), kAbsent);  // a prefix isn't a match
  EXPECT_EQ(Get(mem, "cherry", kMaxSequenceNumber), kAbsent);
}

TEST(MemTableTest, IteratorReturnsEveryVersionInInternalKeyOrder) {
  MemTable mem;
  mem.Add(2, ValueType::kValue, "apple", "green");
  mem.Add(5, ValueType::kDeletion, "apple", "ignored");  // a tombstone stores no value
  mem.Add(6, ValueType::kValue, "banana", "yellow");
  mem.Add(7, ValueType::kValue, "apple", "red");

  struct Entry {
    std::string user_key;
    SequenceNumber seq;
    ValueType type;
    std::string value;
  };
  const std::vector<Entry> expected = {
      {"apple", 7, ValueType::kValue, "red"},
      {"apple", 5, ValueType::kDeletion, ""},
      {"apple", 2, ValueType::kValue, "green"},
      {"banana", 6, ValueType::kValue, "yellow"},
  };

  auto it = mem.NewIterator();
  EXPECT_FALSE(it->Valid());  // unpositioned until a seek
  it->SeekToFirst();
  for (const Entry& e : expected) {
    ASSERT_TRUE(it->Valid());
    const auto parts = DecodeInternalKey(it->key());
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->user_key, e.user_key);
    EXPECT_EQ(parts->sequence, e.seq);
    EXPECT_EQ(parts->type, e.type);
    EXPECT_EQ(it->value(), e.value);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());

  // Seek takes an internal key.
  it->Seek(SeekKey("apple", 6));
  ASSERT_TRUE(it->Valid());
  std::string apple5;
  AppendInternalKey(&apple5, "apple", 5, ValueType::kDeletion);
  EXPECT_EQ(it->key(), apple5);
  EXPECT_OK(it->status());
}

TEST(MemTableTest, AwkwardKeysAndValues) {
  MemTable mem;
  const std::string nul_key("a\0b", 3);
  const std::string high_key("\xff\xfe", 2);
  const std::string long_key(1000, 'k');
  const std::string big_value(100000, 'v');  // bigger than an arena block
  mem.Add(1, ValueType::kValue, "", "empty key");
  mem.Add(2, ValueType::kValue, nul_key, std::string("\0", 1));
  mem.Add(3, ValueType::kValue, high_key, "");
  mem.Add(4, ValueType::kValue, long_key, big_value);
  mem.Add(5, ValueType::kValue, "a", "just a");

  EXPECT_EQ(Get(mem, "", 10), Found("empty key"));
  EXPECT_EQ(Get(mem, nul_key, 10), Found(std::string("\0", 1)));
  EXPECT_EQ(Get(mem, "a", 10), Found("just a"));  // not confused with "a\0b"
  EXPECT_EQ(Get(mem, high_key, 10), Found(""));    // an empty value is still a value
  EXPECT_EQ(Get(mem, long_key, 10), Found(big_value));
}

TEST(MemTableTest, MemoryUsageGrows) {
  MemTable mem;
  const size_t empty = mem.ApproximateMemoryUsage();
  EXPECT_GT(empty, 0u);  // the head node already lives in the arena
  const std::string value(100, 'x');
  for (int i = 0; i < 1000; ++i) {
    mem.Add(static_cast<SequenceNumber>(i + 1), ValueType::kValue, "key" + std::to_string(i), value);
  }
  // 1000 entries of ~110 bytes plus their nodes: at least 110 KB, and the
  // arena wastes well under half.
  const size_t used = mem.ApproximateMemoryUsage();
  EXPECT_GE(used, empty + 1000 * 110);
  EXPECT_LE(used, empty + 1000 * 110 * 2);
}

// ---------------------------------------------------------------------------
// Differential test: a seeded random sequence of Puts, Deletes and Gets at
// random snapshots, checked against a multi-version std::map model. Every
// so often, a full scan and random Seeks are checked too.
//
// Rerun one case with LSMKV_MEMTABLE_SEED=<seed> LSMKV_MEMTABLE_OPS=<n>.
// ---------------------------------------------------------------------------

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::strtoull(v, nullptr, 10) : fallback;
}

// user key -> (sequence -> value, or nullopt for a tombstone)
using Model = std::map<std::string, std::map<SequenceNumber, std::optional<std::string>>>;

Lookup ModelGet(const Model& model, const std::string& key, SequenceNumber snapshot) {
  auto k = model.find(key);
  if (k == model.end()) return kAbsent;
  auto v = k->second.upper_bound(snapshot);  // first version newer than the snapshot
  if (v == k->second.begin()) return kAbsent;
  --v;  // the newest version <= snapshot
  return v->second ? Found(*v->second) : kDeleted;
}

// The model's entries in internal-key order.
std::vector<std::pair<std::string, std::string>> ModelEntries(const Model& model) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [user_key, versions] : model) {
    for (auto v = versions.rbegin(); v != versions.rend(); ++v) {  // newest first
      std::string ikey;
      AppendInternalKey(&ikey, user_key, v->first,
                        v->second ? ValueType::kValue : ValueType::kDeletion);
      out.emplace_back(std::move(ikey), v->second.value_or(""));
    }
  }
  return out;
}

void RunDifferential(uint64_t seed, uint64_t num_ops) {
  std::mt19937_64 rng(seed);
  auto uniform = [&](uint64_t lo, uint64_t hi) {
    return std::uniform_int_distribution<uint64_t>(lo, hi)(rng);
  };

  // Keys: a dense range so keys collect many versions, plus awkward ones.
  std::vector<std::string> keys;
  for (int i = 0; i < 2000; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "user%06d", i);
    keys.push_back(buf);
  }
  keys.push_back("");
  keys.push_back(std::string("\0", 1));
  keys.push_back(std::string("user000001\0", 11));  // a key with another key as a prefix
  keys.push_back("user");
  keys.push_back("\xff\xff\xff");
  keys.push_back(std::string(300, 'L'));

  MemTable mem;
  Model model;
  SequenceNumber seq = 0;
  uint64_t puts = 0, deletes = 0, gets = 0, hits = 0, scans = 0;

  for (uint64_t op = 0; op < num_ops; ++op) {
    const uint64_t roll = uniform(0, 99);
    const std::string& key = keys[uniform(0, keys.size() - 1)];
    if (roll < 45) {  // Put
      const uint64_t r = uniform(0, 999);
      std::string value(r < 50 ? 0 : (r < 995 ? uniform(1, 32) : uniform(33, 1024)), '\0');
      for (char& c : value) c = static_cast<char>(uniform(0, 255));
      mem.Add(++seq, ValueType::kValue, key, value);
      model[key][seq] = value;
      ++puts;
    } else if (roll < 60) {  // Delete
      mem.Add(++seq, ValueType::kDeletion, key, "");
      model[key][seq] = std::nullopt;
      ++deletes;
    } else {  // Get: half at the latest sequence, half at a random older snapshot
      const SequenceNumber snapshot = uniform(0, 1) ? seq : uniform(0, seq);
      const Lookup expected = ModelGet(model, key, snapshot);
      const Lookup actual = Get(mem, key, snapshot);
      ++gets;
      if (expected.result == MemTable::GetResult::kFound) ++hits;
      ASSERT_EQ(actual, expected) << "seed=" << seed << " op=" << op << " key=" << Printable(key)
                                  << " snapshot=" << snapshot << " latest=" << seq;
    }

    if ((op + 1) % 100000 == 0 || op + 1 == num_ops) {
      ++scans;
      const auto entries = ModelEntries(model);
      auto it = mem.NewIterator();
      it->SeekToFirst();
      for (size_t i = 0; i < entries.size(); ++i) {
        ASSERT_TRUE(it->Valid()) << "seed=" << seed << " scan ended early at " << i;
        ASSERT_EQ(it->key(), entries[i].first) << "seed=" << seed << " entry " << i;
        ASSERT_EQ(it->value(), entries[i].second) << "seed=" << seed << " entry " << i;
        it->Next();
      }
      ASSERT_FALSE(it->Valid()) << "seed=" << seed << " scan has extra entries";

      // Seek to random internal keys: land on the model's lower bound.
      for (int s = 0; s < 1000; ++s) {
        std::string target;
        AppendInternalKey(&target, keys[uniform(0, keys.size() - 1)], uniform(0, seq + 1),
                          uniform(0, 1) ? ValueType::kValue : ValueType::kDeletion);
        auto lb = std::lower_bound(entries.begin(), entries.end(), target,
                                   [&](const auto& e, const std::string& t) {
                                     return CompareInternalKeys(e.first, t) < 0;
                                   });
        it->Seek(target);
        if (lb == entries.end()) {
          ASSERT_FALSE(it->Valid()) << "seed=" << seed;
        } else {
          ASSERT_TRUE(it->Valid()) << "seed=" << seed;
          ASSERT_EQ(it->key(), lb->first) << "seed=" << seed;
        }
      }
    }
  }
  std::cout << "[memtable differential] seed=" << seed << " ops=" << num_ops << " puts=" << puts
            << " deletes=" << deletes << " gets=" << gets << " (hits=" << hits
            << ") scans=" << scans << " memory=" << mem.ApproximateMemoryUsage() / 1024
            << " KB\n";
}

TEST(MemTableTest, MatchesMultiVersionModel) {
  const uint64_t num_ops = EnvOr("LSMKV_MEMTABLE_OPS", 1'000'000);
  if (const char* s = std::getenv("LSMKV_MEMTABLE_SEED")) {
    RunDifferential(std::strtoull(s, nullptr, 10), num_ops);
  } else {
    RunDifferential(301, num_ops);
  }
}

// ---------------------------------------------------------------------------
// One writer, four lock-free readers.
//
// Operation i (sequence i + 1) writes key "k<i % kKeys>": a Delete when
// i % 7 == 3, otherwise a Put of a value that encodes i. So for any snapshot,
// the right answer for any key can be computed. After each Add the writer
// bumps `published` (release); a reader only asks about snapshots that are
// already published, where the answer must be exact.
// ---------------------------------------------------------------------------

constexpr uint64_t kConcurrentOps = 50000;
constexpr uint64_t kConcurrentKeys = 500;

bool IsDelete(uint64_t i) { return i % 7 == 3; }
std::string ValueFor(uint64_t i) { return "v" + std::to_string(i) + std::string(i % 40, '.'); }

Lookup ExpectedAt(uint64_t key_id, SequenceNumber snapshot) {
  // Sequence s = i + 1 wrote key i % kKeys. The newest i with i + 1 <= snapshot
  // and i % kKeys == key_id:
  if (snapshot == 0 || snapshot - 1 < key_id) return kAbsent;
  const uint64_t last = snapshot - 1;
  const uint64_t i = last - (last - key_id) % kConcurrentKeys;
  return IsDelete(i) ? kDeleted : Found(ValueFor(i));
}

TEST(MemTableTest, ConcurrentGetsDuringAdds) {
  MemTable mem;
  std::atomic<uint64_t> published{0};  // sequences 1..published are in the memtable
  std::atomic<bool> failed{false};

  auto reader = [&](uint32_t seed) {
    std::mt19937_64 rng(seed);
    uint64_t checks = 0;
    while (!failed.load(std::memory_order_relaxed)) {
      const uint64_t n = published.load(std::memory_order_acquire);
      const uint64_t key_id = rng() % kConcurrentKeys;
      const SequenceNumber snapshot = n == 0 ? 0 : (rng() % 2 ? n : rng() % (n + 1));
      const Lookup actual = Get(mem, "k" + std::to_string(key_id), snapshot);
      const Lookup expected = ExpectedAt(key_id, snapshot);
      if (actual != expected) {
        ADD_FAILURE() << "key k" << key_id << " at snapshot " << snapshot << ": got " << actual
                      << ", want " << expected;
        failed = true;
        return;
      }
      // Now and then, scan a stretch and check it's in order.
      if (++checks % 64 == 0) {
        auto it = mem.NewIterator();
        std::string prev;
        int steps = 0;
        for (it->SeekToFirst(); it->Valid() && steps < 200; it->Next(), ++steps) {
          if (!prev.empty() && CompareInternalKeys(prev, it->key()) >= 0) {
            ADD_FAILURE() << "iterator out of order";
            failed = true;
            return;
          }
          prev.assign(it->key());
        }
      }
      if (n == kConcurrentOps && checks > 2000) return;
    }
  };

  std::vector<std::thread> readers;
  for (uint32_t r = 0; r < 4; ++r) readers.emplace_back(reader, 7 + r);

  for (uint64_t i = 0; i < kConcurrentOps; ++i) {
    const std::string key = "k" + std::to_string(i % kConcurrentKeys);
    if (IsDelete(i)) {
      mem.Add(i + 1, ValueType::kDeletion, key, "");
    } else {
      mem.Add(i + 1, ValueType::kValue, key, ValueFor(i));
    }
    published.store(i + 1, std::memory_order_release);
  }
  for (auto& t : readers) t.join();
  EXPECT_FALSE(failed.load());
}

}  // namespace
}  // namespace lsmkv
