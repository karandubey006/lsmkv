#include "env/fault_injection_env.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace lsmkv {
namespace {

// Drives persist whole sectors, but a multi-sector write can land partially.
constexpr uint64_t kSectorSize = 512;

// Invariant: synced <= data.size().
struct Inode {
  std::string data;
  uint64_t synced = 0;
};

using NameTable = std::map<std::string, std::shared_ptr<Inode>, std::less<>>;

struct Dir {
  NameTable live;
  NameTable durable;
  std::set<std::string, std::less<>> locked;
};

}  // namespace

struct FaultInjectionEnv::State {
  std::mutex mu;
  // Bumped by every power loss. A handle records the epoch it was opened in
  // and is dead once they differ.
  uint64_t epoch = 0;
  std::map<std::string, std::shared_ptr<Dir>> dirs;

  // A scheduled power loss: 0 means none is armed.
  uint64_t mutations_until_loss = 0;
  PowerLossMode scheduled_mode = PowerLossMode::kDropUnsynced;
  uint64_t scheduled_seed = 0;

  int failing_syncs = 0;
};

namespace {

using State = FaultInjectionEnv::State;

Status PowerLost() { return Status::IOError("simulated power loss"); }

void LosePowerLocked(State& state, FaultInjectionEnv::PowerLossMode mode, uint64_t seed);

// Called, with state.mu held, at the start of every mutating operation.
Status BeforeMutation(State& state, uint64_t handle_epoch) {
  if (state.epoch != handle_epoch) return PowerLost();
  if (state.mutations_until_loss > 0 && --state.mutations_until_loss == 0) {
    LosePowerLocked(state, state.scheduled_mode, state.scheduled_seed);
    return PowerLost();
  }
  return Status::OK();
}

class FaultWritableFile final : public WritableFile {
 public:
  FaultWritableFile(std::shared_ptr<State> state, std::shared_ptr<Inode> inode, uint64_t epoch)
      : state_(std::move(state)), inode_(std::move(inode)), epoch_(epoch) {}

  Status Append(std::string_view data) override {
    std::lock_guard<std::mutex> guard(state_->mu);
    Status s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    if (closed_) return Status::IOError("append to closed file");
    inode_->data.append(data);
    return Status::OK();
  }

  Status Sync() override {
    std::lock_guard<std::mutex> guard(state_->mu);
    Status s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    if (closed_) return Status::IOError("sync of closed file");
    if (state_->failing_syncs > 0) {
      --state_->failing_syncs;
      return Status::IOError("injected sync failure");
    }
    inode_->synced = inode_->data.size();
    return Status::OK();
  }

  Status Close() override {
    std::lock_guard<std::mutex> guard(state_->mu);
    closed_ = true;
    return Status::OK();
  }

 private:
  const std::shared_ptr<State> state_;
  const std::shared_ptr<Inode> inode_;
  const uint64_t epoch_;
  bool closed_ = false;  // guarded by state_->mu
};

class FaultReadableFile final : public ReadableFile {
 public:
  FaultReadableFile(std::shared_ptr<State> state, std::shared_ptr<Inode> inode, uint64_t epoch)
      : state_(std::move(state)), inode_(std::move(inode)), epoch_(epoch) {}

  Status ReadAt(uint64_t offset, size_t n, std::string_view* result,
                char* scratch) const override {
    std::lock_guard<std::mutex> guard(state_->mu);
    if (state_->epoch != epoch_) return PowerLost();
    const std::string& data = inode_->data;
    size_t len = 0;
    if (offset < data.size()) len = std::min<uint64_t>(n, data.size() - offset);
    std::copy_n(data.data() + offset, len, scratch);
    *result = std::string_view(scratch, len);
    return Status::OK();
  }

  Status Size(uint64_t* size) const override {
    std::lock_guard<std::mutex> guard(state_->mu);
    if (state_->epoch != epoch_) return PowerLost();
    *size = inode_->data.size();
    return Status::OK();
  }

 private:
  const std::shared_ptr<State> state_;
  const std::shared_ptr<Inode> inode_;
  const uint64_t epoch_;
};

class FaultFileLock final : public FileLock {
 public:
  FaultFileLock(std::shared_ptr<State> state, std::shared_ptr<Dir> dir, std::string name,
                uint64_t epoch)
      : state_(std::move(state)), dir_(std::move(dir)), name_(std::move(name)), epoch_(epoch) {}

  ~FaultFileLock() override {
    std::lock_guard<std::mutex> guard(state_->mu);
    // A power loss already released every lock, and the name may since have
    // been locked again by a new holder.
    if (state_->epoch == epoch_) dir_->locked.erase(name_);
  }

 private:
  const std::shared_ptr<State> state_;
  const std::shared_ptr<Dir> dir_;
  const std::string name_;
  const uint64_t epoch_;
};

class FaultDirectory final : public Directory {
 public:
  FaultDirectory(std::shared_ptr<State> state, std::shared_ptr<Dir> dir, std::string path,
                 uint64_t epoch)
      : state_(std::move(state)), dir_(std::move(dir)), path_(std::move(path)), epoch_(epoch) {}

  Status Create(std::string_view name, std::unique_ptr<WritableFile>* file) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    if (dir_->live.contains(name)) return Status::IOError(Describe(name), "already exists");
    auto inode = std::make_shared<Inode>();
    dir_->live.emplace(std::string(name), inode);
    *file = std::make_unique<FaultWritableFile>(state_, std::move(inode), epoch_);
    return Status::OK();
  }

  Status Open(std::string_view name, std::unique_ptr<ReadableFile>* file) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    if (state_->epoch != epoch_) return PowerLost();
    auto it = dir_->live.find(name);
    if (it == dir_->live.end()) return Status::NotFound(Describe(name));
    *file = std::make_unique<FaultReadableFile>(state_, it->second, epoch_);
    return Status::OK();
  }

  Status Truncate(std::string_view name, uint64_t size) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    auto it = dir_->live.find(name);
    if (it == dir_->live.end()) return Status::NotFound(Describe(name));
    Inode& inode = *it->second;
    if (size > inode.data.size()) {
      return Status::InvalidArgument(Describe(name), "truncate would grow the file");
    }
    inode.data.resize(size);
    inode.synced = size;  // Truncate syncs the whole file
    return Status::OK();
  }

  Status Rename(std::string_view from, std::string_view to) override {
    Status s = CheckFileName(from);
    if (s.ok()) s = CheckFileName(to);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    auto it = dir_->live.find(from);
    if (it == dir_->live.end()) return Status::NotFound(Describe(from));
    std::shared_ptr<Inode> inode = std::move(it->second);
    dir_->live.erase(it);
    dir_->live.insert_or_assign(std::string(to), std::move(inode));
    return Status::OK();
  }

  Status Remove(std::string_view name) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    auto it = dir_->live.find(name);
    if (it == dir_->live.end()) return Status::NotFound(Describe(name));
    dir_->live.erase(it);  // open handles keep the inode alive, as with unlink
    return Status::OK();
  }

  Status List(std::vector<std::string>* names) override {
    names->clear();
    std::lock_guard<std::mutex> guard(state_->mu);
    if (state_->epoch != epoch_) return PowerLost();
    for (const auto& [name, inode] : dir_->live) names->push_back(name);
    return Status::OK();
  }

  Status Lock(std::string_view name, std::unique_ptr<FileLock>* lock) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    std::lock_guard<std::mutex> guard(state_->mu);
    if (state_->epoch != epoch_) return PowerLost();
    if (dir_->locked.contains(name)) return Status::IOError("lock " + Describe(name), "already held");
    if (!dir_->live.contains(name)) dir_->live.emplace(std::string(name), std::make_shared<Inode>());
    dir_->locked.emplace(name);
    *lock = std::make_unique<FaultFileLock>(state_, dir_, std::string(name), epoch_);
    return Status::OK();
  }

  Status Sync() override {
    std::lock_guard<std::mutex> guard(state_->mu);
    Status s = BeforeMutation(*state_, epoch_);
    if (!s.ok()) return s;
    dir_->durable = dir_->live;
    return Status::OK();
  }

  const std::string& path() const override { return path_; }

 private:
  std::string Describe(std::string_view name) const {
    return path_ + "/" + std::string(name);
  }

  const std::shared_ptr<State> state_;
  const std::shared_ptr<Dir> dir_;
  const std::string path_;
  const uint64_t epoch_;
};

void LoseUnsyncedBytes(Inode& inode, FaultInjectionEnv::PowerLossMode mode, std::mt19937_64& rng) {
  const uint64_t synced = inode.synced;
  std::string& data = inode.data;
  if (mode == FaultInjectionEnv::PowerLossMode::kDropUnsynced || data.size() == synced) {
    data.resize(synced);
  } else {
    // The surviving length is drawn so that the edge cases are common: none
    // of the unsynced bytes, all of them (the new size reached the disk; its
    // sectors may not have), or a uniformly random amount.
    const uint64_t unsynced = data.size() - synced;
    uint64_t keep = 0;
    switch (rng() % 3) {
      case 0: keep = synced; break;
      case 1: keep = data.size(); break;
      default: keep = synced + rng() % (unsynced + 1); break;
    }
    data.resize(keep);
    // Sectors are aligned to file offsets; the first may begin before
    // `synced`, but only its unsynced part can be lost.
    for (uint64_t off = synced; off < keep;) {
      const uint64_t sector_end = std::min(keep, (off / kSectorSize + 1) * kSectorSize);
      if (rng() & 1) std::fill(data.begin() + off, data.begin() + sector_end, '\0');
      off = sector_end;
    }
  }
  inode.synced = data.size();  // what survived is now on the media
}

void LosePowerLocked(State& state, FaultInjectionEnv::PowerLossMode mode, uint64_t seed) {
  ++state.epoch;
  state.mutations_until_loss = 0;
  std::mt19937_64 rng(seed);
  // std::map iteration order is sorted, so the rng draws are deterministic.
  // Each inode has at most one name (no hard links), so none is visited twice.
  for (auto& [path, dir] : state.dirs) {
    dir->live = dir->durable;
    dir->locked.clear();
    for (auto& [name, inode] : dir->durable) LoseUnsyncedBytes(*inode, mode, rng);
  }
}

}  // namespace

FaultInjectionEnv::FaultInjectionEnv() : state_(std::make_shared<State>()) {}
FaultInjectionEnv::~FaultInjectionEnv() = default;

Status FaultInjectionEnv::CreateDirectory(const std::string& path) {
  std::lock_guard<std::mutex> guard(state_->mu);
  if (!state_->dirs.try_emplace(path, std::make_shared<Dir>()).second) {
    return Status::IOError(path, "already exists");
  }
  return Status::OK();
}

Status FaultInjectionEnv::OpenDirectory(const std::string& path, std::unique_ptr<Directory>* dir) {
  std::lock_guard<std::mutex> guard(state_->mu);
  auto it = state_->dirs.find(path);
  if (it == state_->dirs.end()) return Status::NotFound(path);
  *dir = std::make_unique<FaultDirectory>(state_, it->second, path, state_->epoch);
  return Status::OK();
}

void FaultInjectionEnv::SimulatePowerLoss(PowerLossMode mode, uint64_t seed) {
  std::lock_guard<std::mutex> guard(state_->mu);
  LosePowerLocked(*state_, mode, seed);
}

void FaultInjectionEnv::SchedulePowerLoss(uint64_t n, PowerLossMode mode, uint64_t seed) {
  std::lock_guard<std::mutex> guard(state_->mu);
  state_->mutations_until_loss = n;
  state_->scheduled_mode = mode;
  state_->scheduled_seed = seed;
}

uint64_t FaultInjectionEnv::power_losses() const {
  std::lock_guard<std::mutex> guard(state_->mu);
  return state_->epoch;
}

void FaultInjectionEnv::FailFileSyncs(int n) {
  std::lock_guard<std::mutex> guard(state_->mu);
  state_->failing_syncs = n;
}

}  // namespace lsmkv
