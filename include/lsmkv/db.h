#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

// An embedded, ordered key-value store. Keys and values are arbitrary byte
// strings (std::string_view, which may contain '\0').
// Keys are ordered byte-wise, comparing bytes as unsigned.
//
// A DB is safe to use from multiple threads at once without external locking.
// A database directory may be opened by only one DB at a time; a second Open
// fails with IOError (enforced with a LOCK file).
class DB {
 public:
  // Opens the database in directory `name`. On success stores it in *dbptr and
  // returns OK. On failure leaves *dbptr unchanged and returns non-OK.
  static Status Open(const Options& options, const std::string& name,
                     std::unique_ptr<DB>* dbptr);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  // Put and Delete are one-operation batches. Every write goes through Write,
  // so the engine has exactly one write path (WAL, sequencing, group commit).
  Status Put(const WriteOptions& options, std::string_view key, std::string_view value);

  // Deleting a key that does not exist is not an error.
  Status Delete(const WriteOptions& options, std::string_view key);

  // Applies every operation in `batch` atomically.
  virtual Status Write(const WriteOptions& options, const WriteBatch& batch) = 0;

  // If the key exists, stores its value in *value and returns OK. If it does
  // not, returns a status for which IsNotFound() is true. An empty value is a
  // present key, distinct from NotFound.
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  // Returns an iterator over a snapshot of the database as of this call.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;
};

}  // namespace lsmkv
