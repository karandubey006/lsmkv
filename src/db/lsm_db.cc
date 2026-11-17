// The engine: writes go to a write-ahead log, then to the active memtable.
//
// Write path (one writer at a time, under write_mu_):
//   1. If the active memtable is full, rotate: sync the current WAL, create
//      the next WAL and sync the directory, then seal the memtable.
//   2. Append the batch to the WAL as one record {first_seq, batch}.
//   3. If WriteOptions::sync, sync the WAL.
//   4. Insert the batch into the active memtable.
//   5. Publish the new last sequence number, making the batch visible.
// Any WAL or directory error is sticky: every later write returns it.
//
// Recovery (Open) replays every WAL in number order. A bad record in the
// newest WAL is a torn tail: replay stops there and the file is truncated to
// its last good record. A bad record in any older WAL, or a well-checksummed
// record that doesn't decode, is Corruption.

#include "db/lsm_db.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <vector>

#include "db/batch_format.h"
#include "db/db_iterator.h"
#include "db/dbformat.h"
#include "db/filenames.h"
#include "db/merging_iterator.h"
#include "env/env.h"
#include "memtable/memtable.h"
#include "wal/wal.h"

namespace lsmkv {
namespace {

// The memtables a reader needs, published as one object so that a reader
// takes a consistent set by copying a single shared_ptr. The set itself never
// changes once published. `active` still receives inserts from the writer,
// which MemTable allows alongside readers; sealed memtables never change.
struct MemTableSet {
  std::shared_ptr<MemTable> active;
  std::vector<std::shared_ptr<MemTable>> sealed;  // newest first
};

// Inserts every op of a well-formed batch, numbering them from first_seq.
// Returns the number of ops.
uint32_t ApplyBatch(std::string_view batch, SequenceNumber first_seq, MemTable& mem) {
  BatchCursor cursor(batch);
  BatchOp op;
  SequenceNumber seq = first_seq;
  while (cursor.Next(&op)) mem.Add(seq++, op.type, op.key, op.value);
  assert(cursor.status().ok());
  return static_cast<uint32_t>(seq - first_seq);
}

class LsmDB final : public DB {
 public:
  LsmDB(const Options& options, std::unique_ptr<Directory> dir, std::unique_ptr<FileLock> lock)
      : options_(options), dir_(std::move(dir)), lock_(std::move(lock)) {}

  ~LsmDB() override {
    if (wal_ != nullptr) static_cast<void>(wal_->Close());  // nothing to report it to
  }

  // Called once, by OpenLsmDB, before the DB is shared.
  Status Recover();

  Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;

 private:
  Status ReplayWal(uint64_t number, bool newest, MemTableSet& mems, SequenceNumber& last);
  Status ReplayRecord(std::string_view payload, MemTableSet& mems, SequenceNumber& last);
  void SealIfFull(MemTableSet& mems) const;
  Status MakeRoomForWrite();
  Status OpenNewWal();

  // Takes a consistent view for a reader.
  void Snapshot(std::shared_ptr<const MemTableSet>* mems, SequenceNumber* seq) {
    std::lock_guard<std::mutex> guard(state_mu_);
    *mems = mems_;
    *seq = last_sequence_;
  }

  const Options options_;
  const std::unique_ptr<Directory> dir_;
  const std::unique_ptr<FileLock> lock_;  // held for the DB's whole lifetime

  // Lock order: write_mu_, then state_mu_. Readers take only state_mu_, and
  // only briefly, so they never wait for WAL I/O.
  std::mutex write_mu_;  // one writer at a time
  std::mutex state_mu_;  // guards publishing mems_ and last_sequence_

  // Written only by the writer, with both mutexes held; read by the writer
  // with write_mu_ held, or by readers with state_mu_ held.
  std::shared_ptr<const MemTableSet> mems_;
  SequenceNumber last_sequence_ = 0;  // newest sequence number visible to readers

  // Guarded by write_mu_.
  std::unique_ptr<WalWriter> wal_;
  uint64_t next_wal_number_ = 1;
  Status sticky_error_;
};

Status LsmDB::Recover() {
  std::vector<std::string> names;
  Status s = dir_->List(&names);
  if (!s.ok()) return s;
  std::vector<uint64_t> wals;
  for (const auto& name : names) {
    if (auto number = ParseWalFileName(name)) wals.push_back(*number);
  }
  std::sort(wals.begin(), wals.end());

  auto mems = std::make_shared<MemTableSet>();
  mems->active = std::make_shared<MemTable>();
  SequenceNumber last = 0;
  for (size_t i = 0; i < wals.size(); ++i) {
    s = ReplayWal(wals[i], /*newest=*/i + 1 == wals.size(), *mems, last);
    if (!s.ok()) return s;
  }

  mems_ = std::move(mems);
  last_sequence_ = last;
  next_wal_number_ = wals.empty() ? 1 : wals.back() + 1;
  return OpenNewWal();
}

Status LsmDB::ReplayWal(uint64_t number, bool newest, MemTableSet& mems, SequenceNumber& last) {
  const std::string name = WalFileName(number);
  std::string contents;
  Status s = ReadEntireFile(*dir_, name, &contents);
  if (!s.ok()) return s;

  WalReader reader(std::move(contents));
  std::string_view payload;
  WalReader::Outcome outcome;
  while ((outcome = reader.Next(&payload)) == WalReader::Outcome::kRecord) {
    s = ReplayRecord(payload, mems, last);
    if (!s.ok()) return Status::Corruption(dir_->path() + "/" + name, s.ToString());
  }
  if (outcome == WalReader::Outcome::kBadRecord && !newest) {
    // Older WALs were synced before the next one was created, so a crash
    // can't have damaged them.
    return Status::Corruption(dir_->path() + "/" + name, reader.bad_reason());
  }
  if (newest) {
    // Drops a torn tail, if any, and syncs what remains. The sync matters even
    // with no tail to drop: replayed records that weren't yet synced are now
    // part of history, so they must be durable before the next WAL takes
    // writes, or a power loss could keep later writes and lose them.
    s = dir_->Truncate(name, reader.good_end());
  }
  return s;
}

Status LsmDB::ReplayRecord(std::string_view payload, MemTableSet& mems, SequenceNumber& last) {
  uint64_t first_seq = 0;
  std::string_view batch;
  Status s = DecodeWalPayload(payload, &first_seq, &batch);
  if (s.ok()) s = ValidateBatch(batch);  // validate all of it before applying any of it
  if (!s.ok()) return s;
  if (first_seq != last + 1) return Status::Corruption("WAL sequence numbers are not contiguous");
  const uint32_t count = BatchCursor(batch).count();
  if (count > kMaxSequenceNumber - last) return Status::Corruption("WAL sequence number overflow");

  SealIfFull(mems);
  last += ApplyBatch(batch, first_seq, *mems.active);
  return Status::OK();
}

void LsmDB::SealIfFull(MemTableSet& mems) const {
  if (mems.active->ApproximateMemoryUsage() < options_.memtable_size) return;
  mems.sealed.insert(mems.sealed.begin(), std::move(mems.active));
  mems.active = std::make_shared<MemTable>();
}

// REQUIRES: write_mu_ held, or the DB not yet shared.
Status LsmDB::OpenNewWal() {
  std::unique_ptr<WritableFile> file;
  Status s = dir_->Create(WalFileName(next_wal_number_++), &file);
  // The new file's name must be durable before a synced write in it is
  // acknowledged.
  if (s.ok()) s = dir_->Sync();
  if (!s.ok()) return s;
  wal_ = std::make_unique<WalWriter>(std::move(file));
  return Status::OK();
}

// REQUIRES: write_mu_ held.
Status LsmDB::MakeRoomForWrite() {
  if (mems_->active->ApproximateMemoryUsage() < options_.memtable_size) return Status::OK();

  // The old WAL is synced before the new one exists. Otherwise a synced write
  // in the new WAL could survive a power loss that loses earlier writes still
  // unsynced in the old one.
  Status s = wal_->Sync();
  if (s.ok()) s = wal_->Close();
  if (s.ok()) s = OpenNewWal();
  if (!s.ok()) return s;

  auto next = std::make_shared<MemTableSet>(*mems_);
  SealIfFull(*next);
  std::lock_guard<std::mutex> guard(state_mu_);
  mems_ = std::move(next);
  return Status::OK();
}

Status LsmDB::Write(const WriteOptions& options, const WriteBatch& batch) {
  const uint32_t count = batch.Count();
  if (count == 0) return Status::OK();
  if (batch.Encoded().size() > kWalMaxPayload - kWalPayloadHeaderSize) {
    return Status::InvalidArgument("write batch too large");
  }

  std::lock_guard<std::mutex> writer(write_mu_);
  if (!sticky_error_.ok()) return sticky_error_;
  if (count > kMaxSequenceNumber - last_sequence_) {
    return Status::InvalidArgument("sequence numbers exhausted");
  }

  Status s = MakeRoomForWrite();
  const SequenceNumber first_seq = last_sequence_ + 1;
  if (s.ok()) s = wal_->AddRecord({EncodeWalPayloadHeader(first_seq), batch.Encoded()});
  if (s.ok() && options.sync) s = wal_->Sync();
  if (!s.ok()) {
    sticky_error_ = s;
    return s;
  }

  // Readers can't see these entries yet: their sequence numbers are above
  // last_sequence_, which every reader's snapshot is capped at.
  ApplyBatch(batch.Encoded(), first_seq, *mems_->active);
  std::lock_guard<std::mutex> guard(state_mu_);
  last_sequence_ = first_seq + count - 1;
  return Status::OK();
}

Status LsmDB::Get(const ReadOptions& /*options*/, std::string_view key, std::string* value) {
  std::shared_ptr<const MemTableSet> mems;
  SequenceNumber snapshot = 0;
  Snapshot(&mems, &snapshot);

  auto lookup = [&](const MemTable& mem) { return mem.Get(key, snapshot, value); };
  MemTable::GetResult result = lookup(*mems->active);
  for (size_t i = 0; result == MemTable::GetResult::kAbsent && i < mems->sealed.size(); ++i) {
    result = lookup(*mems->sealed[i]);
  }
  if (result == MemTable::GetResult::kFound) return Status::OK();
  return Status::NotFound(key);
}

std::unique_ptr<Iterator> LsmDB::NewIterator(const ReadOptions& /*options*/) {
  std::shared_ptr<const MemTableSet> mems;
  SequenceNumber snapshot = 0;
  Snapshot(&mems, &snapshot);

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mems->active->NewIterator());
  for (const auto& mem : mems->sealed) children.push_back(mem->NewIterator());
  // The iterator holds the set, so its memtables outlive it.
  return NewDBIterator(mems, NewMergingIterator(std::move(children)), snapshot);
}

}  // namespace

Status OpenLsmDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr) {
  Env* env = options.env != nullptr ? options.env : Env::Posix();

  std::unique_ptr<Directory> dir;
  Status s = env->OpenDirectory(name, &dir);
  if (s.ok()) {
    if (options.error_if_exists) {
      return Status::InvalidArgument(name, "exists (error_if_exists is true)");
    }
  } else if (s.IsNotFound()) {
    if (!options.create_if_missing) {
      return Status::InvalidArgument(name, "does not exist (create_if_missing is false)");
    }
    s = env->CreateDirectory(name);
    if (s.ok()) s = env->OpenDirectory(name, &dir);
  }
  if (!s.ok()) return s;

  std::unique_ptr<FileLock> lock;
  s = dir->Lock(kLockFileName, &lock);
  if (!s.ok()) return s;

  auto db = std::make_unique<LsmDB>(options, std::move(dir), std::move(lock));
  s = db->Recover();
  if (!s.ok()) return s;
  *dbptr = std::move(db);
  return Status::OK();
}

}  // namespace lsmkv
