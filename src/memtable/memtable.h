#pragma once

// In-memory sorted table of recent writes. Every Put and Delete adds an entry;
// nothing is overwritten or removed.
//
// Each entry is one arena allocation:
//   internal_key | varint32 value_size | value
// The skip list holds a view of the internal key; the value follows it.
//
// Thread safety: Add() requires external synchronization. Get() and iterators
// may run concurrently with Add().
//
// Lifetime: iterators and the views they return point into the MemTable, which
// must outlive them.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"
#include "memtable/arena.h"
#include "memtable/skiplist.h"

namespace lsmkv {

class MemTable {
 public:
  enum class GetResult {
    kFound,    // *value holds the value
    kDeleted,  // the newest visible entry is a tombstone
    kAbsent,   // no visible entry; older data must be checked
  };

  MemTable() : table_(EntryCompare{}, &arena_) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // REQUIRES: external synchronization.
  // REQUIRES: sequence is unique within this MemTable.
  void Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
           std::string_view value);

  // Looks up the newest entry for user_key with sequence <= snapshot.
  GetResult Get(std::string_view user_key, SequenceNumber snapshot, std::string* value) const;

  // Yields every entry, including tombstones and older versions, in
  // internal-key order. key() is the internal key; Seek() takes one.
  std::unique_ptr<Iterator> NewIterator() const;

  // Safe to call from any thread.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

 private:
  class TableIterator;

  struct EntryCompare {
    int operator()(std::string_view a, std::string_view b) const {
      return CompareInternalKeys(a, b);
    }
  };
  using Table = SkipList<std::string_view, EntryCompare>;

  Arena arena_;  // declared before table_, which allocates from it
  Table table_;
};

}  // namespace lsmkv
