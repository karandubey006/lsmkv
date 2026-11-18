#pragma once

#include <array>
#include <memory>
#include <vector>

#include "db/version_edit.h"
#include "lsmkv/status.h"

namespace lsmkv {

// The set of live SSTs, by level. Immutable once built, so it is shared
// between threads as std::shared_ptr<const Version> without locking.
//
// L0 files may overlap one another and are ordered newest first (a higher
// file number is newer). Files in each level >= 1 have disjoint internal-key
// ranges and are ordered by smallest key.
class Version {
 public:
  using FileList = std::vector<std::shared_ptr<const SstInfo>>;

  Version() = default;  // no files

  // REQUIRES: 0 <= level < kNumLevels.
  const FileList& files(int level) const { return levels_[level]; }

  size_t TotalFiles() const;

  // Builds in *result the version that follows from applying edit to this
  // one: removals first, then additions, so a file may move between levels
  // under the same number. Corruption, with *result untouched, if the edit
  // removes a file that isn't at that level, adds a number that is already
  // present, adds a file whose keys are malformed or out of order, or leaves
  // two files with overlapping ranges in a level >= 1. The edit's counters
  // are ignored.
  Status Apply(const VersionEdit& edit, std::shared_ptr<const Version>* result) const;

 private:
  std::array<FileList, kNumLevels> levels_;
};

}  // namespace lsmkv
