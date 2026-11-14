#pragma once

#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

// A forward iterator over key/value pairs in ascending byte-wise key order.
//
// An iterator sees a fixed snapshot of the database as of its creation: writes
// made after NewIterator() returns are invisible to it.
//
// A new iterator is not positioned (Valid() is false) until a Seek call.
//
// Forward-only for now. Prev()/SeekToLast() can be added later without
// changing any existing call site.
//
// The views returned by key() and value() are valid only until the next call
// that moves the iterator, or until the iterator is destroyed.
//
// An iterator must be destroyed before the DB that created it.
class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;

  // Position at the first key in the snapshot.
  virtual void SeekToFirst() = 0;

  // Position at the first key >= target.
  virtual void Seek(std::string_view target) = 0;

  // Advance to the next key. REQUIRES: Valid().
  virtual void Next() = 0;

  // REQUIRES: Valid().
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  // An error the iterator hit (e.g. Corruption). If !Valid() and status() is
  // OK, iteration simply reached the end.
  virtual Status status() const = 0;
};

}  // namespace lsmkv
