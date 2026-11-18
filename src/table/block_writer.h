#pragma once

// Block contents format, shared by data and index blocks:
//
//   block   := entry*  restart:fixed32 * num_restarts  num_restarts:fixed32
//   entry   := shared:varint32  unshared:varint32  value_len:varint32
//              key_suffix[unshared]  value[value_len]
//
// An entry's key is the first `shared` bytes of the previous entry's key
// followed by key_suffix. Every restart_interval-th entry, starting with the
// first, is a restart point: it stores its whole key (shared = 0) and its
// offset is listed in the restart array, so a reader can binary-search the
// restart keys and decode forward from there.
//
// Keys are internal keys (db/dbformat.h), strictly increasing under
// CompareInternalKeys. Restart offsets are strictly increasing and the first
// is 0. A block with no entries is encoded as a single restart at offset 0
// (8 bytes); it is valid, and is what an empty table's index block holds.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmkv {

// Builds one block. Not thread-safe.
class BlockWriter {
 public:
  // REQUIRES: restart_interval >= 1.
  explicit BlockWriter(int restart_interval);
  BlockWriter(const BlockWriter&) = delete;
  BlockWriter& operator=(const BlockWriter&) = delete;

  // REQUIRES: Finish() not called since the last Reset(); key is a
  // well-formed internal key greater than every key added before it.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block. The view stays
  // valid until Reset() or destruction.
  std::string_view Finish();

  // Starts a new, empty block, keeping the allocated buffers.
  void Reset();

  // The size the block would have if finished now.
  size_t EstimatedSize() const;

  bool empty() const { return num_entries_ == 0; }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  std::string last_key_;
  size_t num_entries_ = 0;
  int since_restart_ = 0;  // entries added since the last restart point
  bool finished_ = false;
};

}  // namespace lsmkv
