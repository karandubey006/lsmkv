#include "db/version.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "db/dbformat.h"

namespace lsmkv {
namespace {

Status CheckKeys(const SstInfo& f) {
  if (!DecodeInternalKey(f.smallest) || !DecodeInternalKey(f.largest)) {
    return Status::Corruption("SST " + std::to_string(f.number), "malformed key range");
  }
  if (CompareInternalKeys(f.smallest, f.largest) > 0) {
    return Status::Corruption("SST " + std::to_string(f.number), "smallest key after largest");
  }
  return Status::OK();
}

}  // namespace

size_t Version::TotalFiles() const {
  size_t n = 0;
  for (const FileList& level : levels_) n += level.size();
  return n;
}

Status Version::Apply(const VersionEdit& edit, std::shared_ptr<const Version>* result) const {
  auto next = std::make_shared<Version>(*this);

  for (const VersionEdit::RemovedFile& r : edit.removed) {
    if (r.level < 0 || r.level >= kNumLevels) return Status::Corruption("level out of range");
    FileList& files = next->levels_[r.level];
    auto it = std::find_if(files.begin(), files.end(),
                           [&](const auto& f) { return f->number == r.number; });
    if (it == files.end()) {
      return Status::Corruption("removed SST " + std::to_string(r.number),
                                "not in level " + std::to_string(r.level));
    }
    files.erase(it);
  }

  std::unordered_set<uint64_t> numbers;
  for (const FileList& files : next->levels_) {
    for (const auto& f : files) numbers.insert(f->number);
  }
  std::array<bool, kNumLevels> grew{};
  for (const VersionEdit::AddedFile& a : edit.added) {
    if (a.level < 0 || a.level >= kNumLevels) return Status::Corruption("level out of range");
    if (Status s = CheckKeys(a.info); !s.ok()) return s;
    if (!numbers.insert(a.info.number).second) {
      return Status::Corruption("added SST " + std::to_string(a.info.number), "already present");
    }
    next->levels_[a.level].push_back(std::make_shared<const SstInfo>(a.info));
    grew[a.level] = true;
  }

  // Removing files keeps a level sorted and disjoint, so only levels that
  // gained files need sorting and checking.
  if (grew[0]) {
    FileList& l0 = next->levels_[0];
    std::sort(l0.begin(), l0.end(),
              [](const auto& a, const auto& b) { return a->number > b->number; });
  }
  for (int level = 1; level < kNumLevels; ++level) {
    if (!grew[level]) continue;
    FileList& files = next->levels_[level];
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
      return CompareInternalKeys(a->smallest, b->smallest) < 0;
    });
    for (size_t i = 1; i < files.size(); ++i) {
      if (CompareInternalKeys(files[i - 1]->largest, files[i]->smallest) >= 0) {
        return Status::Corruption(
            "SSTs " + std::to_string(files[i - 1]->number) + " and " +
                std::to_string(files[i]->number),
            "overlap in level " + std::to_string(level));
      }
    }
  }

  *result = std::move(next);
  return Status::OK();
}

}  // namespace lsmkv
