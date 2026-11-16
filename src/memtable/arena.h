#pragma once

// Bump-pointer allocator. Memory is released only when the Arena is destroyed.
//
// Thread safety: Allocate() requires external synchronization. MemoryUsage()
// may be called from any thread.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsmkv {

class Arena {
 public:
  static constexpr size_t kBlockSize = 4096;

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns `bytes` bytes aligned to `align`.
  // REQUIRES: bytes > 0; align is a power of two no larger than
  // alignof(std::max_align_t).
  char* Allocate(size_t bytes, size_t align = 1);

  // Total size of all blocks.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  char* NewBlock(size_t size);

  // Free space in the current block is [cursor_, limit_).
  char* cursor_ = nullptr;
  char* limit_ = nullptr;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

}  // namespace lsmkv
