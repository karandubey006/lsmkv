#include "db/map_db.h"

#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "env/env.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {
namespace {

// std::less<> is a transparent comparator, so find/lower_bound accept a
// string_view without first building a std::string. std::string's ordering
// compares bytes as unsigned char, which is the byte-wise order lsmkv promises.
using Map = std::map<std::string, std::string, std::less<>>;

// One immutable-once-shared copy of the data. Iterators pin a Version so their
// snapshot stays alive and unchanged; a write that finds the current Version
// pinned copies it first (copy-on-write). This is a toy version of the
// reference-counted Versions that Phase 5 builds for SSTables.
struct Version {
  Map map;
  // The one field that still changes after a Version is shared, so it is
  // mutable and guarded by MapDB::mu_ instead of frozen by const.
  mutable int pinned_by_iterators = 0;
};

class MapDB;

class SnapshotIterator final : public Iterator {
 public:
  SnapshotIterator(MapDB* db, std::shared_ptr<const Version> version)
      : db_(db), version_(std::move(version)), it_(version_->map.end()) {}
  ~SnapshotIterator() override;

  bool Valid() const override { return it_ != version_->map.end(); }
  void SeekToFirst() override { it_ = version_->map.begin(); }
  void Seek(std::string_view target) override { it_ = version_->map.lower_bound(target); }
  void Next() override { ++it_; }
  std::string_view key() const override { return it_->first; }
  std::string_view value() const override { return it_->second; }
  Status status() const override { return Status::OK(); }

 private:
  MapDB* const db_;
  const std::shared_ptr<const Version> version_;
  Map::const_iterator it_;
};

class MapDB final : public DB {
 public:
  explicit MapDB(std::unique_ptr<FileLock> lock)
      : lock_(std::move(lock)), current_(std::make_shared<Version>()) {}

  Status Write(const WriteOptions& /*options*/, const WriteBatch& batch) override {
    // Decode the whole batch before touching the map, so a malformed batch
    // applies nothing (atomicity). The views point into `batch`, which outlives
    // this call.
    struct Op {
      bool is_put;
      std::string_view key;
      std::string_view value;
    };
    struct Stager final : WriteBatch::Handler {
      std::vector<Op> ops;
      void Put(std::string_view k, std::string_view v) override { ops.push_back({true, k, v}); }
      void Delete(std::string_view k) override { ops.push_back({false, k, {}}); }
    } stager;
    Status s = batch.Iterate(stager);
    if (!s.ok()) return s;

    // Holding mu_ for the whole batch means no reader sees half of it.
    std::lock_guard<std::mutex> guard(mu_);
    if (current_->pinned_by_iterators > 0) {
      auto copy = std::make_shared<Version>();
      copy->map = current_->map;
      current_ = std::move(copy);  // the old Version lives on in its iterators
    }
    Map& map = current_->map;
    for (const Op& op : stager.ops) {
      if (op.is_put) {
        map.insert_or_assign(std::string(op.key), std::string(op.value));
      } else if (auto it = map.find(op.key); it != map.end()) {
        map.erase(it);
      }
    }
    return Status::OK();
  }

  Status Get(const ReadOptions& /*options*/, std::string_view key, std::string* value) override {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = current_->map.find(key);
    if (it == current_->map.end()) return Status::NotFound("key not found");
    value->assign(it->second);
    return Status::OK();
  }

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& /*options*/) override {
    std::lock_guard<std::mutex> guard(mu_);
    ++current_->pinned_by_iterators;
    return std::make_unique<SnapshotIterator>(this, current_);
  }

  void Unpin(const Version& version) {
    std::lock_guard<std::mutex> guard(mu_);
    --version.pinned_by_iterators;
  }

 private:
  const std::unique_ptr<FileLock> lock_;  // held for the DB's whole lifetime
  std::mutex mu_;
  std::shared_ptr<Version> current_;  // guarded by mu_
};

// Unpinning under the DB's mutex orders every read this iterator made before
// any later in-place write to the same Version.
SnapshotIterator::~SnapshotIterator() { db_->Unpin(*version_); }

}  // namespace

Status OpenMapDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr) {
  Env* env = options.env != nullptr ? options.env : Env::Default();

  if (env->FileExists(name)) {
    if (options.error_if_exists) {
      return Status::InvalidArgument(name, "exists (error_if_exists is true)");
    }
  } else {
    if (!options.create_if_missing) {
      return Status::InvalidArgument(name, "does not exist (create_if_missing is false)");
    }
    Status s = env->CreateDir(name);
    if (!s.ok()) return s;
  }

  std::unique_ptr<FileLock> lock;
  Status s = env->LockFile(name + "/LOCK", &lock);
  if (!s.ok()) return s;

  *dbptr = std::make_unique<MapDB>(std::move(lock));
  return Status::OK();
}

}  // namespace lsmkv
