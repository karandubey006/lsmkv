// Microbenchmark: MemTable (skip list + arena) vs std::map<std::string, std::string>.
//
// Same keys (16 random bytes, inserted in random order) and values (100 bytes)
// for both. The map stores one version per key; the memtable stores internal
// keys (user key + 8-byte trailer), so it does slightly more work per entry.
//
//   make bench-micro

#include <benchmark/benchmark.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "memtable/memtable.h"

namespace lsmkv {
namespace {

std::vector<std::string> RandomKeys(size_t n) {
  std::mt19937_64 rng(42);
  std::vector<std::string> keys(n, std::string(16, '\0'));
  for (auto& k : keys) {
    for (char& c : k) c = static_cast<char>(rng());
  }
  return keys;
}

const std::string kValue(100, 'v');

void BM_MemTableInsert(benchmark::State& state) {
  const auto keys = RandomKeys(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    auto* mem = new MemTable;
    SequenceNumber seq = 0;
    for (const auto& k : keys) mem->Add(++seq, ValueType::kValue, k, kValue);
    benchmark::DoNotOptimize(mem->ApproximateMemoryUsage());
    state.PauseTiming();  // don't time the teardown
    delete mem;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_StdMapInsert(benchmark::State& state) {
  const auto keys = RandomKeys(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    auto* map = new std::map<std::string, std::string>;
    for (const auto& k : keys) map->insert_or_assign(k, kValue);
    benchmark::DoNotOptimize(map->size());
    state.PauseTiming();
    delete map;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_MemTableGet(benchmark::State& state) {
  const auto keys = RandomKeys(static_cast<size_t>(state.range(0)));
  MemTable mem;
  SequenceNumber seq = 0;
  for (const auto& k : keys) mem.Add(++seq, ValueType::kValue, k, kValue);

  std::mt19937_64 rng(7);
  std::string value;
  for (auto _ : state) {
    const auto& k = keys[rng() % keys.size()];
    benchmark::DoNotOptimize(mem.Get(k, kMaxSequenceNumber, &value));
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_StdMapGet(benchmark::State& state) {
  const auto keys = RandomKeys(static_cast<size_t>(state.range(0)));
  std::map<std::string, std::string> map;
  for (const auto& k : keys) map.insert_or_assign(k, kValue);

  std::mt19937_64 rng(7);
  std::string value;
  for (auto _ : state) {
    const auto& k = keys[rng() % keys.size()];
    auto it = map.find(k);
    value.assign(it->second);  // copy out, as MemTable::Get does
    benchmark::DoNotOptimize(value.data());
  }
  state.SetItemsProcessed(state.iterations());
}

// 40K entries is about a 4 MB memtable of 100-byte values; 400K is a 40 MB one.
BENCHMARK(BM_MemTableInsert)->Arg(40'000)->Arg(400'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_StdMapInsert)->Arg(40'000)->Arg(400'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MemTableGet)->Arg(40'000)->Arg(400'000);
BENCHMARK(BM_StdMapGet)->Arg(40'000)->Arg(400'000);

}  // namespace
}  // namespace lsmkv

BENCHMARK_MAIN();
