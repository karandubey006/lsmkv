// A large write-then-reopen run on the real disk. Skipped unless
// LSMKV_LARGE_MB is set, e.g. LSMKV_LARGE_MB=1024 for about 1 GB of user data.
// Best run on the release build:
//   LSMKV_LARGE_MB=1024 ./build/release/lsmkv_tests --gtest_filter='LargeTest.*'

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include "lsmkv/db.h"
#include "test_util.h"

namespace lsmkv {
namespace {

constexpr size_t kValueSize = 100;

// Distinct keys in a scrambled order (an odd multiplier is a bijection on
// 64-bit integers), so every flush covers keys from the whole key space.
std::string KeyFor(uint64_t i) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(i * 0x9E3779B97F4A7C15ull));
  return buf;
}

std::string ValueFor(uint64_t i) {
  std::string value = std::to_string(i) + ":";
  value.resize(kValueSize, static_cast<char>('a' + i % 26));
  return value;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

TEST(LargeTest, WriteReopenReadBack) {
  const char* mb = std::getenv("LSMKV_LARGE_MB");
  if (mb == nullptr) GTEST_SKIP() << "set LSMKV_LARGE_MB to run";
  const uint64_t n = std::strtoull(mb, nullptr, 10) * (1 << 20) / (16 + kValueSize);

  test::TempDir dir;
  const std::string path = dir.path() + "/db";
  Options options;
  options.create_if_missing = true;

  auto start = std::chrono::steady_clock::now();
  {
    std::unique_ptr<DB> db;
    ASSERT_OK(DB::Open(options, path, &db));
    for (uint64_t i = 0; i < n; ++i) ASSERT_OK(db->Put(WriteOptions(), KeyFor(i), ValueFor(i)));
  }
  std::cout << "wrote " << n << " keys in " << SecondsSince(start) << " s\n";

  size_t ssts = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (entry.path().extension() == ".sst") ++ssts;
  }
  std::cout << ssts << " SSTs\n";
  EXPECT_GT(ssts, 1u);

  start = std::chrono::steady_clock::now();
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, path, &db));
  std::cout << "reopened in " << SecondsSince(start) << " s\n";

  start = std::chrono::steady_clock::now();
  uint64_t count = 0;
  std::string prev;
  auto it = db->NewIterator(ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++count) {
    ASSERT_TRUE(count == 0 || it->key() > prev);
    prev.assign(it->key());
  }
  ASSERT_OK(it->status());
  EXPECT_EQ(count, n);
  std::cout << "scanned in " << SecondsSince(start) << " s\n";

  start = std::chrono::steady_clock::now();
  std::mt19937_64 rng(1);
  std::string value;
  for (int k = 0; k < 2000; ++k) {
    const uint64_t i = rng() % n;
    ASSERT_OK(db->Get(ReadOptions(), KeyFor(i), &value));
    ASSERT_EQ(value, ValueFor(i));
  }
  EXPECT_TRUE(db->Get(ReadOptions(), "absent", &value).IsNotFound());
  std::cout << "2000 random gets in " << SecondsSince(start) << " s\n";
}

}  // namespace
}  // namespace lsmkv
