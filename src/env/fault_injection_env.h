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
// surviving file back to its synced bytes (or, in kTornWrites mode, keeps a
// random mangling of the unsynced bytes). Directories made by
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
    // run of its unsynced bytes, in which each 4 KB page independently
    // survives or reads back as zeros. Models torn writes, zero-filled tails
    // and out-of-order page writeback.
    kTornWrites,
  };

  FaultInjectionEnv();
  ~FaultInjectionEnv() override;

  Status CreateDirectory(const std::string& path) override;
  Status OpenDirectory(const std::string& path, std::unique_ptr<Directory>* dir) override;

  // Deterministic for a given seed and sequence of prior operations.
  void SimulatePowerLoss(PowerLossMode mode, uint64_t seed);

  struct State;  // public only so the handle classes in the .cc can name it

 private:
  // Shared with every handle, so a handle that outlives the Env stays safe.
  std::shared_ptr<State> state_;
};

}  // namespace lsmkv
