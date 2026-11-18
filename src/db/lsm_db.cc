// The engine: writes go to a write-ahead log, then to the active memtable. A
// full memtable is sealed and a background thread flushes it to a level-0 SST.
//
// Write path (one writer at a time, under write_mu_):
//   1. If the active memtable is full, rotate: wait until no sealed memtable
//      is waiting to be flushed (a write stall), sync and close the current
//      WAL, create the next WAL and sync the directory, then seal the
//      memtable and wake the flush thread.
//   2. Append the batch to the WAL as one record {first_seq, batch}.
//   3. If WriteOptions::sync, sync the WAL.
//   4. Insert the batch into the active memtable.
//   5. Publish the new last sequence number, making the batch visible.
// Any WAL or directory error is sticky: every later write returns it.
//
// Flush (background thread), in this order:
//   1. Write the sealed memtable to a new SST.
//   2. Sync the SST: its bytes are durable.
//   3. Sync the directory: its name is durable.
//   4. Commit a MANIFEST edit adding the SST to level 0 and moving the log
//      number past the sealed memtable's WAL. Once the edit is synced the
//      flush has happened; before that, recovery still replays the WAL.
//   5. Publish the new version and drop the sealed memtable.
//   6. Remove WALs below the log number.
// A failed flush is sticky: every later write returns the error, and the WAL
// stays, so the next Open recovers the data.
//
// Recovery (Open):
//   1. Load the MANIFEST that CURRENT names, then write a fresh one.
//   2. Replay every WAL numbered >= the log number, in order. A bad record in
//      the newest of them is a torn tail and ends replay. A bad record in any
//      older one, or a well-checksummed record that doesn't decode, is
//      Corruption.
//   3. Flush everything replayed to level-0 SSTs and commit them with a log
//      number past every replayed WAL.
//   4. Create the new WAL, then remove obsolete files.

#include "db/lsm_db.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "db/batch_format.h"
#include "db/db_iterator.h"
#include "db/dbformat.h"
#include "db/filenames.h"
#include "db/manifest.h"
#include "db/merging_iterator.h"
#include "db/version.h"
#include "db/version_edit.h"
#include "env/env.h"
#include "memtable/memtable.h"
#include "table/sst_reader.h"
#include "table/sst_writer.h"
#include "wal/wal.h"

namespace lsmkv {
namespace {

// A memtable waiting to be flushed.
struct SealedMemTable {
  std::shared_ptr<MemTable> mem;
  uint64_t next_log_number;      // the WAL that took writes after it was sealed
  SequenceNumber last_sequence;  // the newest sequence number in mem
};

// Everything a reader needs, published as one immutable object so a reader
// takes a consistent view by copying a single shared_ptr. `active` still
// receives inserts from the writer, which MemTable allows alongside readers;
// nothing else in a view changes once it is published.
struct ReadView {
  std::shared_ptr<MemTable> active;
  std::optional<SealedMemTable> sealed;  // at most one
  std::shared_ptr<const Version> version;
  // tables[level][i] reads version->files(level)[i].
  std::array<std::vector<std::shared_ptr<const SstReader>>, kNumLevels> tables;
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

bool IsEmpty(const MemTable& mem) {
  std::unique_ptr<Iterator> it = mem.NewIterator();
  it->SeekToFirst();
  return !it->Valid();
}

class LsmDB final : public DB {
 public:
  LsmDB(const Options& options, std::unique_ptr<Directory> dir, std::unique_ptr<FileLock> lock)
      : options_(options), dir_(std::move(dir)), lock_(std::move(lock)), manifest_(dir_.get()) {}

  ~LsmDB() override {
    {
      std::lock_guard<std::mutex> guard(state_mu_);
      shutting_down_ = true;
    }
    flush_wanted_.notify_all();
    // A flush in progress finishes; a sealed memtable not yet started stays
    // in its WAL and is recovered by the next Open.
    if (flush_thread_.joinable()) flush_thread_.join();
    if (wal_ != nullptr) static_cast<void>(wal_->Close());  // nothing to report it to
  }

  // Called once, by OpenLsmDB, before the DB is shared.
  Status Recover();

  Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;

  // Seals the active memtable if it holds anything, and waits until no
  // flush is pending or running. Returns the background error, if any.
  Status FlushForTesting();

 private:
  // Recovery.
  Status ReplayWal(uint64_t number, bool newest,
                   std::vector<std::shared_ptr<MemTable>>& mems, SequenceNumber& last);
  Status ReplayRecord(std::string_view payload, std::vector<std::shared_ptr<MemTable>>& mems,
                      SequenceNumber& last);
  Status OpenLiveTables();

  // Write path. REQUIRES: write_mu_ held.
  Status MakeRoomForWrite(bool force);
  Status OpenNewWal(uint64_t number);

  // Flushing.
  void FlushLoop();
  Status FlushSealed(const SealedMemTable& sealed);
  Status WriteSst(const MemTable& mem, VersionEdit* edit);
  void PublishVersion(bool drop_sealed);
  void RemoveObsoleteFiles();

  // Takes a consistent view for a reader.
  void Snapshot(std::shared_ptr<const ReadView>* view, SequenceNumber* seq) {
    std::lock_guard<std::mutex> guard(state_mu_);
    *view = view_;
    *seq = last_sequence_;
  }

  const Options options_;
  const std::unique_ptr<Directory> dir_;
  const std::unique_ptr<FileLock> lock_;  // held for the DB's whole lifetime

  // Lock order: write_mu_, then flush_mu_, then state_mu_. Readers take only
  // state_mu_, and only briefly, so they never wait for I/O.
  std::mutex write_mu_;  // one writer at a time
  std::mutex flush_mu_;  // guards manifest_ and tables_
  std::mutex state_mu_;  // guards the fields below that readers see
  std::condition_variable flush_wanted_;  // with state_mu_: a memtable was sealed
  std::condition_variable flush_done_;    // with state_mu_: a flush ended

  // Guarded by flush_mu_.
  Manifest manifest_;
  std::map<uint64_t, std::shared_ptr<const SstReader>> tables_;  // every live SST, by number

  // Guarded by state_mu_. view_ is replaced, never modified, so a reader may
  // keep using a copy after unlocking. last_sequence_ is written only by the
  // writer, which may also read it holding just write_mu_.
  std::shared_ptr<const ReadView> view_;
  SequenceNumber last_sequence_ = 0;  // newest sequence number visible to readers
  Status bg_error_;                   // first flush failure; once set, kept
  bool flush_running_ = false;
  bool shutting_down_ = false;

  // Guarded by write_mu_.
  std::shared_ptr<MemTable> active_;  // the same memtable as view_->active
  std::unique_ptr<WalWriter> wal_;
  Status sticky_error_;

  std::thread flush_thread_;  // started at the end of Recover
};

Status LsmDB::Recover() {
  bool exists = false;
  Status s = manifest_.Recover(&exists);
  if (!s.ok()) return s;

  std::vector<std::string> names;
  s = dir_->List(&names);
  if (!s.ok()) return s;
  std::vector<uint64_t> wals;
  for (const auto& name : names) {
    const std::optional<ParsedFileName> parsed = ParseFileName(name);
    if (!parsed) continue;
    if (!exists && (parsed->kind == FileKind::kWal || parsed->kind == FileKind::kSst)) {
      return Status::Corruption(dir_->path(), "has data files but no CURRENT");
    }
    if (parsed->kind == FileKind::kWal && parsed->number >= manifest_.log_number()) {
      wals.push_back(parsed->number);
    }
  }
  std::sort(wals.begin(), wals.end());

  s = exists ? manifest_.Rewrite() : manifest_.Create();
  if (s.ok()) s = OpenLiveTables();
  if (!s.ok()) return s;

  std::vector<std::shared_ptr<MemTable>> mems{std::make_shared<MemTable>()};
  SequenceNumber last = manifest_.last_sequence();
  for (size_t i = 0; i < wals.size(); ++i) {
    s = ReplayWal(wals[i], /*newest=*/i + 1 == wals.size(), mems, last);
    if (!s.ok()) return s;
  }

  // Flush what was replayed, oldest first so newer data gets higher file
  // numbers, and commit it along with a log number past every replayed WAL.
  // Until that commit the WALs are still what recovery reads, so a crash
  // before it leaves them as they were.
  VersionEdit edit;
  for (const auto& mem : mems) {
    s = WriteSst(*mem, &edit);
    if (!s.ok()) return s;
  }
  const uint64_t wal_number = manifest_.NewFileNumber();
  edit.log_number = wal_number;
  edit.last_sequence = last;
  s = manifest_.Commit(&edit);
  if (s.ok()) s = OpenNewWal(wal_number);
  if (!s.ok()) return s;

  active_ = std::make_shared<MemTable>();
  last_sequence_ = last;
  auto view = std::make_shared<ReadView>();
  view->active = active_;
  view_ = std::move(view);
  PublishVersion(/*drop_sealed=*/false);
  RemoveObsoleteFiles();

  flush_thread_ = std::thread([this] { FlushLoop(); });
  return Status::OK();
}

Status LsmDB::ReplayWal(uint64_t number, bool newest,
                        std::vector<std::shared_ptr<MemTable>>& mems, SequenceNumber& last) {
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
  // A torn tail in the newest WAL is left in place: recovery flushes what
  // came before it and then moves the log number past this WAL.
  return Status::OK();
}

Status LsmDB::ReplayRecord(std::string_view payload, std::vector<std::shared_ptr<MemTable>>& mems,
                           SequenceNumber& last) {
  uint64_t first_seq = 0;
  std::string_view batch;
  Status s = DecodeWalPayload(payload, &first_seq, &batch);
  if (s.ok()) s = ValidateBatch(batch);  // validate all of it before applying any of it
  if (!s.ok()) return s;
  if (first_seq != last + 1) return Status::Corruption("WAL sequence numbers are not contiguous");
  const uint32_t count = BatchCursor(batch).count();
  if (count > kMaxSequenceNumber - last) return Status::Corruption("WAL sequence number overflow");

  if (mems.back()->ApproximateMemoryUsage() >= options_.memtable_size) {
    mems.push_back(std::make_shared<MemTable>());
  }
  last += ApplyBatch(batch, first_seq, *mems.back());
  return Status::OK();
}

// Opens a reader for every SST in the recovered version.
// REQUIRES: the DB not yet shared.
Status LsmDB::OpenLiveTables() {
  const std::shared_ptr<const Version> version = manifest_.current();
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& info : version->files(level)) {
      std::unique_ptr<ReadableFile> file;
      Status s = dir_->Open(SstFileName(info->number), &file);
      if (s.IsNotFound()) {
        return Status::Corruption(dir_->path(), "missing " + SstFileName(info->number));
      }
      std::unique_ptr<SstReader> reader;
      if (s.ok()) s = SstReader::Open(std::move(file), info->file_size, &reader);
      if (!s.ok()) return s;
      tables_[info->number] = std::move(reader);
    }
  }
  return Status::OK();
}

// REQUIRES: write_mu_ held, or the DB not yet shared.
Status LsmDB::OpenNewWal(uint64_t number) {
  std::unique_ptr<WritableFile> file;
  Status s = dir_->Create(WalFileName(number), &file);
  // The new file's name must be durable before a synced write in it is
  // acknowledged.
  if (s.ok()) s = dir_->Sync();
  if (!s.ok()) return s;
  wal_ = std::make_unique<WalWriter>(std::move(file));
  return Status::OK();
}

// REQUIRES: write_mu_ held.
Status LsmDB::MakeRoomForWrite(bool force) {
  if (force ? IsEmpty(*active_)
            : active_->ApproximateMemoryUsage() < options_.memtable_size) {
    return Status::OK();
  }

  {
    std::unique_lock<std::mutex> lock(state_mu_);
    flush_done_.wait(lock, [this] { return !view_->sealed.has_value() || !bg_error_.ok(); });
    if (!bg_error_.ok()) return bg_error_;
  }

  // The old WAL is synced before the new one exists. Otherwise a synced write
  // in the new WAL could survive a power loss that loses earlier writes still
  // unsynced in the old one.
  Status s = wal_->Sync();
  if (s.ok()) s = wal_->Close();
  uint64_t number = 0;
  {
    std::lock_guard<std::mutex> guard(flush_mu_);
    number = manifest_.NewFileNumber();
  }
  if (s.ok()) s = OpenNewWal(number);
  if (!s.ok()) return s;

  // last_sequence_ is the newest entry in the old memtable: rotation happens
  // between batches.
  SealedMemTable sealed{std::move(active_), number, last_sequence_};
  active_ = std::make_shared<MemTable>();
  {
    std::lock_guard<std::mutex> guard(state_mu_);
    auto next = std::make_shared<ReadView>(*view_);
    next->active = active_;
    next->sealed = std::move(sealed);
    view_ = std::move(next);
  }
  flush_wanted_.notify_one();
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
  {
    std::lock_guard<std::mutex> guard(state_mu_);
    if (!bg_error_.ok()) return bg_error_;
  }
  if (count > kMaxSequenceNumber - last_sequence_) {
    return Status::InvalidArgument("sequence numbers exhausted");
  }

  Status s = MakeRoomForWrite(/*force=*/false);
  const SequenceNumber first_seq = last_sequence_ + 1;
  if (s.ok()) s = wal_->AddRecord({EncodeWalPayloadHeader(first_seq), batch.Encoded()});
  if (s.ok() && options.sync) s = wal_->Sync();
  if (!s.ok()) {
    sticky_error_ = s;
    return s;
  }

  // Readers can't see these entries yet: their sequence numbers are above
  // last_sequence_, which every reader's snapshot is capped at.
  ApplyBatch(batch.Encoded(), first_seq, *active_);
  std::lock_guard<std::mutex> guard(state_mu_);
  last_sequence_ = first_seq + count - 1;
  return Status::OK();
}

Status LsmDB::FlushForTesting() {
  std::lock_guard<std::mutex> writer(write_mu_);
  if (!sticky_error_.ok()) return sticky_error_;
  Status s = MakeRoomForWrite(/*force=*/true);
  if (!s.ok()) {
    sticky_error_ = s;
    return s;
  }
  std::unique_lock<std::mutex> lock(state_mu_);
  flush_done_.wait(lock, [this] {
    return (!view_->sealed.has_value() && !flush_running_) || !bg_error_.ok();
  });
  return bg_error_;
}

void LsmDB::FlushLoop() {
  std::unique_lock<std::mutex> lock(state_mu_);
  while (true) {
    flush_wanted_.wait(lock, [this] {
      return shutting_down_ || (view_->sealed.has_value() && bg_error_.ok());
    });
    if (shutting_down_) return;
    const SealedMemTable sealed = *view_->sealed;
    flush_running_ = true;
    lock.unlock();
    Status s = FlushSealed(sealed);
    lock.lock();
    flush_running_ = false;
    if (!s.ok()) bg_error_ = s;
    flush_done_.notify_all();
  }
}

// Runs on the flush thread.
Status LsmDB::FlushSealed(const SealedMemTable& sealed) {
  VersionEdit edit;
  Status s = WriteSst(*sealed.mem, &edit);
  if (!s.ok()) return s;
  edit.log_number = sealed.next_log_number;
  edit.last_sequence = sealed.last_sequence;
  {
    std::lock_guard<std::mutex> guard(flush_mu_);
    s = manifest_.Commit(&edit);  // the commit point
    if (!s.ok()) return s;
    PublishVersion(/*drop_sealed=*/true);
  }
  RemoveObsoleteFiles();
  return Status::OK();
}

// Writes mem to a new SST, syncs it and the directory, opens a reader for it,
// and records it in *edit as a level-0 file. Does nothing for an empty
// memtable. On error the file, if any, is left as an orphan that the next
// Open removes.
Status LsmDB::WriteSst(const MemTable& mem, VersionEdit* edit) {
  std::unique_ptr<Iterator> it = mem.NewIterator();
  it->SeekToFirst();
  if (!it->Valid()) return Status::OK();

  uint64_t number = 0;
  {
    std::lock_guard<std::mutex> guard(flush_mu_);
    number = manifest_.NewFileNumber();
  }
  const std::string name = SstFileName(number);
  std::unique_ptr<WritableFile> file;
  Status s = dir_->Create(name, &file);
  if (!s.ok()) return s;

  SstWriterOptions writer_options;
  writer_options.block_size = options_.block_size;
  writer_options.restart_interval = options_.block_restart_interval;
  SstWriter writer(writer_options, file.get());
  for (; it->Valid() && s.ok(); it->Next()) s = writer.Add(it->key(), it->value());
  if (s.ok()) {
    s = writer.Finish();
  } else {
    writer.Abandon();
  }
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  if (s.ok()) s = dir_->Sync();

  std::unique_ptr<ReadableFile> readable;
  std::unique_ptr<SstReader> reader;
  if (s.ok()) s = dir_->Open(name, &readable);
  if (s.ok()) s = SstReader::Open(std::move(readable), writer.FileSize(), &reader);
  if (!s.ok()) return s;

  edit->AddFile(0, SstInfo{number, writer.FileSize(), writer.smallest_key(), writer.largest_key()});
  std::lock_guard<std::mutex> guard(flush_mu_);
  tables_[number] = std::move(reader);
  return Status::OK();
}

// Publishes manifest_.current() with a reader for each of its files.
// REQUIRES: flush_mu_ held, or the DB not yet shared.
void LsmDB::PublishVersion(bool drop_sealed) {
  std::shared_ptr<const Version> version = manifest_.current();
  std::array<std::vector<std::shared_ptr<const SstReader>>, kNumLevels> tables;
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& info : version->files(level)) {
      const auto it = tables_.find(info->number);
      assert(it != tables_.end());
      tables[level].push_back(it->second);
    }
  }

  {
    std::lock_guard<std::mutex> guard(state_mu_);
    auto next = std::make_shared<ReadView>(*view_);
    next->version = std::move(version);
    next->tables = std::move(tables);
    if (drop_sealed) next->sealed.reset();
    view_ = std::move(next);
  }
  flush_done_.notify_all();
}

// Removes WALs below the log number, SSTs not in the current version (left
// by a failed flush), MANIFESTs other than the current one, and a stale
// CURRENT.tmp. Removal failures are ignored: a leftover file is harmless and
// the next call or Open retries it.
//
// REQUIRES: no SST is being written concurrently. It runs only on the flush
// thread, or during Open before that thread starts.
void LsmDB::RemoveObsoleteFiles() {
  uint64_t log_number = 0;
  uint64_t manifest_number = 0;
  {
    std::lock_guard<std::mutex> guard(flush_mu_);
    log_number = manifest_.log_number();
    manifest_number = manifest_.manifest_number();
  }

  std::vector<std::string> names;
  if (!dir_->List(&names).ok()) return;
  for (const auto& name : names) {
    const std::optional<ParsedFileName> parsed = ParseFileName(name);
    if (!parsed) continue;
    bool obsolete = false;
    switch (parsed->kind) {
      case FileKind::kWal:
        obsolete = parsed->number < log_number;
        break;
      case FileKind::kSst: {
        // Every SST in the current version has a reader. So may one whose
        // commit failed; that error is sticky, and the next Open removes it.
        std::lock_guard<std::mutex> guard(flush_mu_);
        obsolete = tables_.count(parsed->number) == 0;
        break;
      }
      case FileKind::kManifest:
        obsolete = parsed->number != manifest_number;
        break;
      case FileKind::kCurrentTmp:
        obsolete = true;
        break;
      case FileKind::kLock:
      case FileKind::kCurrent:
        break;
    }
    if (obsolete) static_cast<void>(dir_->Remove(name));
  }
}

Status LsmDB::Get(const ReadOptions& /*options*/, std::string_view key, std::string* value) {
  std::shared_ptr<const ReadView> view;
  SequenceNumber snapshot = 0;
  Snapshot(&view, &snapshot);

  MemTable::GetResult result = view->active->Get(key, snapshot, value);
  if (result == MemTable::GetResult::kAbsent && view->sealed) {
    result = view->sealed->mem->Get(key, snapshot, value);
  }

  // Level 0 newest first, then each deeper level. Files whose key range
  // can't hold the key are skipped; in levels >= 1 at most one can.
  const std::string target = SeekKey(key, snapshot);
  std::string found_key;
  for (int level = 0; result == MemTable::GetResult::kAbsent && level < kNumLevels; ++level) {
    const Version::FileList& files = view->version->files(level);
    for (size_t i = 0; result == MemTable::GetResult::kAbsent && i < files.size(); ++i) {
      if (key < UserKeyOf(files[i]->smallest) || key > UserKeyOf(files[i]->largest)) continue;
      bool found = false;
      Status s = view->tables[level][i]->SeekEntry(target, &found, &found_key, value);
      if (!s.ok()) return s;
      if (!found || UserKeyOf(found_key) != key) continue;
      result = TypeOf(found_key) == ValueType::kValue ? MemTable::GetResult::kFound
                                                       : MemTable::GetResult::kDeleted;
    }
  }
  if (result == MemTable::GetResult::kFound) return Status::OK();
  return Status::NotFound(key);
}

std::unique_ptr<Iterator> LsmDB::NewIterator(const ReadOptions& /*options*/) {
  std::shared_ptr<const ReadView> view;
  SequenceNumber snapshot = 0;
  Snapshot(&view, &snapshot);

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(view->active->NewIterator());
  if (view->sealed) children.push_back(view->sealed->mem->NewIterator());
  for (const auto& level : view->tables) {
    for (const auto& table : level) children.push_back(table->NewIterator());
  }
  // The iterator holds the view, so its memtables and tables outlive it.
  return NewDBIterator(view, NewMergingIterator(std::move(children)), snapshot);
}

}  // namespace

Status OpenLsmDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr) {
  if (options.block_size == 0 || options.block_restart_interval < 1) {
    return Status::InvalidArgument("block_size and block_restart_interval must be positive");
  }
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

Status FlushForTesting(DB& db) { return static_cast<LsmDB&>(db).FlushForTesting(); }

}  // namespace lsmkv
