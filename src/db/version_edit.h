#pragma once

// A VersionEdit describes the change from one Version to the next: files
// added to and removed from levels, and new values for the counters the
// MANIFEST records. The MANIFEST is a log of encoded edits.
//
// Encoding: a sequence of fields, in any order, each a varint32 tag followed
// by that tag's value. Tags are persisted; never renumber them.
//
//   tag 1  added file    level:varint32  number:varint64  file_size:varint64
//                        smallest:length-prefixed  largest:length-prefixed
//   tag 2  removed file  level:varint32  number:varint64
//   tag 3  log number        varint64
//   tag 4  next file number  varint64
//   tag 5  last sequence     varint64
//
// smallest and largest are internal keys. Tags 3-5 appear at most once per
// edit; tags 1 and 2 any number of times.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Number of levels in the tree: L0 .. L(kNumLevels-1).
inline constexpr int kNumLevels = 7;

// What the engine knows about one SST without opening it.
struct SstInfo {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal key of the first entry
  std::string largest;   // internal key of the last entry

  bool operator==(const SstInfo&) const = default;
};

struct VersionEdit {
  struct AddedFile {
    int level;
    SstInfo info;
    bool operator==(const AddedFile&) const = default;
  };
  struct RemovedFile {
    int level;
    uint64_t number;
    bool operator==(const RemovedFile&) const = default;
  };

  // The oldest WAL that still holds data not yet in an SST. WALs with smaller
  // numbers are obsolete.
  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file_number;
  std::optional<SequenceNumber> last_sequence;
  std::vector<AddedFile> added;
  std::vector<RemovedFile> removed;

  // REQUIRES: 0 <= level < kNumLevels.
  void AddFile(int level, SstInfo info) { added.push_back({level, std::move(info)}); }
  void RemoveFile(int level, uint64_t number) { removed.push_back({level, number}); }

  // Appends the encoding of this edit to *dst. An empty edit encodes as no
  // bytes. REQUIRES: every level is in range.
  void EncodeTo(std::string* dst) const;

  // Replaces *this with the edit encoded in src. Corruption if src is not
  // exactly one well-formed edit (unknown tag, truncated field, level out of
  // range, repeated counter, trailing bytes). On error *this is unspecified.
  Status DecodeFrom(std::string_view src);

  bool operator==(const VersionEdit&) const = default;
};

}  // namespace lsmkv
