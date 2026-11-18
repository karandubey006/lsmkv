#pragma once

#include <cstddef>

namespace lsmkv {

class Env;

// Options for DB::Open. Fields are added in the phase that first uses them.
struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = false;

  // Fail Open if the database already exists.
  bool error_if_exists = false;

  // All file I/O goes through this. nullptr means Env::Posix().
  // Not owned: it must outlive the DB. Tests pass a fault-injection Env here.
  Env* env = nullptr;

  // Once the in-memory table of recent writes reaches about this many bytes,
  // it is sealed and a new one (with a new write-ahead log) takes writes.
  size_t memtable_size = 4 << 20;

  // Target size of a data block in a table file. A block is closed once it
  // reaches this size, so it can run over by one entry.
  size_t block_size = 4 << 10;

  // Number of entries between restart points in a data block: a full key is
  // stored every this many entries, and the rest are prefix-compressed.
  int block_restart_interval = 16;
};

// Per-write options.
struct WriteOptions {
  // If true, the write is synced to the write-ahead log before Put/Delete/
  // Write returns, so an acknowledged write survives power loss. If false, it
  // survives a process crash but may be lost on power loss. A synced write
  // also makes every earlier write durable.
  bool sync = false;
};

// Per-read options. Currently empty.
struct ReadOptions {};

}  // namespace lsmkv
