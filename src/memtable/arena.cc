#include "memtable/arena.h"

#include <cassert>
#include <cstdint>

namespace lsmkv {
namespace {

size_t PaddingFor(const char* p, size_t align) {
  const auto addr = reinterpret_cast<uintptr_t>(p);
  return (align - (addr & (align - 1))) & (align - 1);
}

}  // namespace

char* Arena::Allocate(size_t bytes, size_t align) {
  assert(bytes > 0);
  assert(align > 0 && (align & (align - 1)) == 0 && align <= alignof(std::max_align_t));

  if (cursor_ != nullptr) {
    const size_t padding = PaddingFor(cursor_, align);
    if (padding + bytes <= static_cast<size_t>(limit_ - cursor_)) {
      char* result = cursor_ + padding;
      cursor_ = result + bytes;
      return result;
    }
  }

  // Blocks from new[] are aligned to max_align_t, so no padding is needed below.
  if (bytes > kBlockSize / 4) {
    // Give large allocations their own block and keep the current one.
    return NewBlock(bytes);
  }
  char* block = NewBlock(kBlockSize);
  cursor_ = block + bytes;
  limit_ = block + kBlockSize;
  return block;
}

char* Arena::NewBlock(size_t size) {
  blocks_.push_back(std::make_unique_for_overwrite<char[]>(size));
  memory_usage_.fetch_add(size, std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace lsmkv
