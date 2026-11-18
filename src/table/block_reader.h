#pragma once

// Reads a block in the format described in block_writer.h.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/iterator.h"
#include "lsmkv/status.h"

namespace lsmkv {

// An immutable, parsed block. Thread-safe: any number of iterators may read
// it concurrently.
//
// Never reads outside its contents, whatever bytes they hold: Parse checks
// the restart array, and iterators check every entry as they decode it,
// reporting a malformed one as Corruption.
class BlockReader {
 public:
  // Takes ownership of contents. Corruption if the restart array is
  // malformed.
  static Status Parse(std::string contents, std::unique_ptr<BlockReader>* out);

  BlockReader(const BlockReader&) = delete;
  BlockReader& operator=(const BlockReader&) = delete;

  // An iterator over the block's entries. key() is the internal key; Seek
  // takes an internal key and lands on the first entry >= it. The iterator
  // must not outlive this BlockReader.
  std::unique_ptr<Iterator> NewIterator() const;

  // Like NewIterator, but the iterator keeps the block alive itself.
  static std::unique_ptr<Iterator> NewIterator(std::shared_ptr<const BlockReader> block);

  size_t size() const { return contents_.size(); }
  uint32_t num_restarts() const { return num_restarts_; }

 private:
  class EntryIterator;

  BlockReader(std::string contents, uint32_t entries_end, uint32_t num_restarts);

  uint32_t RestartOffset(uint32_t index) const;

  const std::string contents_;
  const uint32_t entries_end_;   // entries occupy [0, entries_end_)
  const uint32_t num_restarts_;  // >= 1
};

}  // namespace lsmkv
