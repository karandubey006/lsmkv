#pragma once

// An in-memory Env that can simulate power loss. Test-only; not part of the
// engine library.
//
// Model:
//   - A file is an inode: its bytes, plus how many of them have been synced.
//   - A directory has two name tables. `live` is what operations see;
//     `durable` is a copy of `live` taken at the directory's last Sync().
//
// SimulatePowerLoss() resets every directory's live names to its durable
// names, so unsynced creates, renames and removes are undone, and cuts every
// surviving file back to its synced bytes. In kTornWrites mode each directory
// instead keeps a random prefix of its unsynced name changes, and each file
// a random mangling of its unsynced bytes. Directories made by
// CreateDirectory() are durable on return, as with the POSIX Env.
//
// Handles (directories, files, locks) opened before a power loss belong to
// the "dead" process: every later operation on them fails with IOError, and
// destroying them is safe.
//
// Thread-safe: one mutex guards all state.

#include <cstdint>
#include <memory>
#include <string>

#include "env/env.h"

namespace lsmkv {

class FaultInjectionEnv final : public Env {
 public:
  enum class PowerLossMode {
    // Every file keeps exactly its synced bytes.
    kDropUnsynced,
    // Every file keeps its synced bytes plus none, all, or a random-length
    // run of its unsynced bytes, in which each 512-byte sector independently
    // survives or reads back as zeros. Models torn writes (cut mid-record),
    // zero-filled tails and out-of-order writeback. Every directory keeps its
    // synced names plus a random prefix (possibly none, possibly all) of its
    // later creates, renames and removes, in the order they were made.
    kTornWrites,
  };

  FaultInjectionEnv();
  ~FaultInjectionEnv() override;

  Status CreateDirectory(const std::string& path) override;
  Status OpenDirectory(const std::string& path, std::unique_ptr<Directory>* dir) override;

  // Deterministic for a given seed and sequence of prior operations.
  void SimulatePowerLoss(PowerLossMode mode, uint64_t seed);

  // Arms a power loss that strikes just before the nth mutating operation
  // from now (n >= 1). Mutating operations are file Append and Sync, and
  // directory Create, Truncate, Rename, Remove and Sync. The operation that
  // triggers it fails, so a crash can land between any two steps of a write.
  void SchedulePowerLoss(uint64_t n, PowerLossMode mode, uint64_t seed);

  // Number of power losses so far, simulated or scheduled.
  uint64_t power_losses() const;

  // The next n file Syncs fail with IOError and sync nothing. With a suffix,
  // only Syncs of files whose name ends with it count.
  void FailFileSyncs(int n, std::string name_suffix = "");

  struct State;  // public only so the handle classes in the .cc can name it

 private:
  // Shared with every handle, so a handle that outlives the Env stays safe.
  std::shared_ptr<State> state_;
};

}  // namespace lsmkv
