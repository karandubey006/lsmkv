#include "memtable/arena.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace lsmkv {
namespace {

TEST(ArenaTest, Empty) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0u);
}

TEST(ArenaTest, SmallAllocationsShareABlock) {
  Arena arena;
  char* a = arena.Allocate(10);
  char* b = arena.Allocate(20);
  EXPECT_EQ(b, a + 10);  // bump pointer: the next allocation starts where the last ended
  EXPECT_EQ(arena.MemoryUsage(), Arena::kBlockSize);
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  Arena arena;
  arena.Allocate(1);  // knock the bump pointer off alignment
  for (int i = 0; i < 1000; ++i) {
    // Powers of two up to the largest supported alignment (8 or 16, by platform).
    const size_t align = std::min(size_t{1} << (i % 5), alignof(std::max_align_t));
    char* p = arena.Allocate(static_cast<size_t>(i % 37 + 1), align);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % align, 0u) << i;
    arena.Allocate(static_cast<size_t>(i % 5 + 1));
  }
}

TEST(ArenaTest, LargeAllocationGetsItsOwnBlock) {
  Arena arena;
  char* small1 = arena.Allocate(100);
  char* big = arena.Allocate(Arena::kBlockSize);  // > kBlockSize / 4
  char* small2 = arena.Allocate(100);
  // The current block kept its free tail: small2 follows small1 directly.
  EXPECT_EQ(small2, small1 + 100);
  EXPECT_NE(big, nullptr);
  EXPECT_EQ(arena.MemoryUsage(), 2 * Arena::kBlockSize);
}

TEST(ArenaTest, RandomAllocationsKeepTheirContents) {
  // Fill every allocation with a pattern derived from its index, then check
  // none were overwritten by later allocations (ASan also catches overruns).
  std::mt19937 rng(301);
  Arena arena;
  std::vector<std::pair<size_t, char*>> allocated;
  size_t total = 0;
  for (int i = 0; i < 20000; ++i) {
    size_t n = (i % 2000 == 0)   ? static_cast<size_t>(i)  // occasionally large
               : (rng() % 20 == 0) ? rng() % 6000
                                   : rng() % 20;
    if (n == 0) n = 1;  // Allocate(0) is not allowed
    char* p = arena.Allocate(n, (rng() % 10 == 0) ? 8 : 1);
    std::memset(p, static_cast<char>(i % 256), n);
    allocated.emplace_back(n, p);
    total += n;
    EXPECT_GE(arena.MemoryUsage(), total);
    // Waste is bounded: at most ~1/4 of each block.
    EXPECT_LE(arena.MemoryUsage(), total * 2 + Arena::kBlockSize * 2);
  }
  for (size_t i = 0; i < allocated.size(); ++i) {
    const auto [n, p] = allocated[i];
    for (size_t j = 0; j < n; ++j) {
      ASSERT_EQ(p[j], static_cast<char>(i % 256)) << "allocation " << i << " byte " << j;
    }
  }
}

}  // namespace
}  // namespace lsmkv
