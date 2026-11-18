#pragma once

// The MANIFEST records which SSTs make up the database and the counters
// needed to reopen it.
//
// On disk:
//   MANIFEST-NNNNNN  records in the WAL framing (wal/wal.h); each payload is
//                    one encoded VersionEdit (db/version_edit.h). The first
//                    record is a snapshot: every counter and every live file.
//   CURRENT          the name of the MANIFEST in use, then '\n'.
//
// A MANIFEST only grows by appending synced edits, so after a crash it can be
// damaged only in its unsynced tail, which holds no committed edit. Recovery
// therefore stops quietly at the first bad record. Each Open writes a fresh
// MANIFEST (Rewrite) rather than appending to the recovered one, so a torn
// tail is never written after.
//
// Switching CURRENT to MANIFEST-N (Create and Rewrite), in order:
//   1. Create MANIFEST-N, append the snapshot, sync it.
//   2. Remove a stale CURRENT.tmp left by an earlier crash, if any.
//   3. Create CURRENT.tmp holding "MANIFEST-N\n", sync it, close it.
//   4. Sync the directory: MANIFEST-N's name is durable before any rename
//      that could make CURRENT name it.
//   5. Rename CURRENT.tmp over CURRENT.
//   6. Sync the directory: the switch is durable.
// A crash before step 5 reaches the disk leaves CURRENT naming the previous
// MANIFEST; after it, CURRENT names a complete, synced MANIFEST-N.

#include <cstdint>
#include <memory>

#include "db/dbformat.h"
#include "db/version.h"
#include "db/version_edit.h"
#include "env/env.h"
#include "lsmkv/status.h"
#include "wal/wal.h"

namespace lsmkv {

// Owns the current Version, the counters, and the MANIFEST being appended to.
//
// Not thread-safe: the caller serializes every call, including current().
// The Versions it hands out are immutable and safe to use from any thread.
//
// Errors are sticky: once an append or sync of the MANIFEST fails, or Create
// or Rewrite fails, every later Commit and Rewrite returns that error, since
// the MANIFEST may end in a partial record and whether the last change is
// durable is unknown. The database must be reopened.
class Manifest {
 public:
  // dir is not owned and must outlive this object.
  explicit Manifest(Directory* dir);
  Manifest(const Manifest&) = delete;
  Manifest& operator=(const Manifest&) = delete;
  ~Manifest();

  // Starts a new, empty database: writes a first MANIFEST and points CURRENT
  // at it. Counters start at log number 0 and last sequence 0. File numbers
  // of files already in the directory (left by an interrupted Create) are
  // never reused. InvalidArgument if CURRENT exists.
  // REQUIRES: a newly constructed Manifest.
  Status Create();

  // Loads the state recorded by the MANIFEST that CURRENT names. Sets
  // *exists to false and returns OK if there is no CURRENT. Corruption if
  // CURRENT isn't exactly a MANIFEST name plus '\n', names a missing file,
  // or the MANIFEST holds a well-checksummed record that doesn't decode or
  // apply, or never sets every counter. A bad record ends replay: it and
  // everything after it are ignored.
  //
  // Every numbered file found in the directory is marked used. Recover does
  // not open the MANIFEST for writing: call Rewrite before Commit.
  // REQUIRES: a newly constructed Manifest.
  Status Recover(bool* exists);

  // Writes a new MANIFEST holding a snapshot of the current state and points
  // CURRENT at it. On OK, manifest_number() is the new MANIFEST's number, and
  // every other MANIFEST in the directory is obsolete and may be removed.
  // REQUIRES: Create or Recover returned OK (with *exists true).
  Status Rewrite();

  // Durably applies edit. Sets edit->next_file_number; the caller sets
  // log_number and last_sequence when they change. On OK the edit is synced
  // to the MANIFEST and current() and the counters reflect it.
  //
  // Not sticky, and changes nothing: Corruption if the edit doesn't apply to
  // current() (see Version::Apply); InvalidArgument if it lowers log_number
  // or last_sequence, adds a file whose number NewFileNumber() never
  // returned, or no MANIFEST is open for writing.
  Status Commit(VersionEdit* edit);

  // Allocates a file number for a new WAL, SST or MANIFEST. It is recorded
  // durably by the next Commit, Create or Rewrite.
  uint64_t NewFileNumber() { return next_file_number_++; }

  // Ensures NewFileNumber() never returns number or anything below it.
  void MarkFileNumberUsed(uint64_t number);

  uint64_t next_file_number() const { return next_file_number_; }
  uint64_t log_number() const { return log_number_; }
  SequenceNumber last_sequence() const { return last_sequence_; }

  // The MANIFEST that CURRENT names: the one recovered or most recently
  // written. 0 before Create or Recover.
  uint64_t manifest_number() const { return manifest_number_; }

  std::shared_ptr<const Version> current() const { return current_; }

 private:
  Status MarkExistingFilesUsed(bool* has_current);
  Status WriteNewManifest();
  Status PointCurrentAt(uint64_t manifest_number);

  Directory* const dir_;
  std::shared_ptr<const Version> current_;
  uint64_t next_file_number_ = 1;
  uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  uint64_t manifest_number_ = 0;
  std::unique_ptr<WalWriter> writer_;  // null until Create or Rewrite succeeds
  Status error_;                       // first failure; once set, returned forever
};

}  // namespace lsmkv
