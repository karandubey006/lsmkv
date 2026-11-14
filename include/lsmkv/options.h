#pragma once

namespace lsmkv {

class Env;

// Options for DB::Open. Fields are added in the phase that first uses them.
struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = false;

  // Fail Open if the database already exists.
  bool error_if_exists = false;

  // All file I/O goes through this. nullptr means Env::Default() (POSIX).
  // Not owned: it must outlive the DB. Tests pass a fault-injection Env here.
  Env* env = nullptr;
};

// Per-write options (design decision D3: options structs from day one).
struct WriteOptions {
  // If true, the write is fsynced to the WAL before Put/Delete/Write returns,
  // so an acknowledged write survives power loss. If false, it survives a
  // process crash but may be lost on power loss.
  //
  // Phase 0: accepted but has no effect (the stand-in engine has no WAL).
  bool sync = false;
};

// Per-read options. Fields (snapshot, verify_checksums, fill_cache) arrive in
// Phase 4; the struct exists now so no call site changes when they do.
struct ReadOptions {};

}  // namespace lsmkv
