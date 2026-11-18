#include "db/version_edit.h"

#include <cassert>

#include "util/coding.h"

namespace lsmkv {
namespace {

// Persisted. Never renumber; see the format in version_edit.h.
enum Tag : uint32_t {
  kTagAddedFile = 1,
  kTagRemovedFile = 2,
  kTagLogNumber = 3,
  kTagNextFileNumber = 4,
  kTagLastSequence = 5,
};

bool GetLevel(std::string_view* input, int* level) {
  uint32_t value = 0;
  if (!GetVarint32(input, &value) || value >= static_cast<uint32_t>(kNumLevels)) return false;
  *level = static_cast<int>(value);
  return true;
}

// Reads a counter that may appear only once per edit.
bool GetCounter(std::string_view* input, std::optional<uint64_t>* field) {
  uint64_t value = 0;
  if (field->has_value() || !GetVarint64(input, &value)) return false;
  *field = value;
  return true;
}

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kTagLogNumber);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kTagNextFileNumber);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kTagLastSequence);
    PutVarint64(dst, *last_sequence);
  }
  for (const RemovedFile& f : removed) {
    assert(f.level >= 0 && f.level < kNumLevels);
    PutVarint32(dst, kTagRemovedFile);
    PutVarint32(dst, static_cast<uint32_t>(f.level));
    PutVarint64(dst, f.number);
  }
  for (const AddedFile& f : added) {
    assert(f.level >= 0 && f.level < kNumLevels);
    PutVarint32(dst, kTagAddedFile);
    PutVarint32(dst, static_cast<uint32_t>(f.level));
    PutVarint64(dst, f.info.number);
    PutVarint64(dst, f.info.file_size);
    PutLengthPrefixed(dst, f.info.smallest);
    PutLengthPrefixed(dst, f.info.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  *this = VersionEdit();
  std::string_view input = src;
  while (!input.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) return Status::Corruption("version edit", "bad tag");
    switch (tag) {
      case kTagAddedFile: {
        AddedFile f{};
        std::string_view smallest, largest;
        if (!GetLevel(&input, &f.level) || !GetVarint64(&input, &f.info.number) ||
            !GetVarint64(&input, &f.info.file_size) || !GetLengthPrefixed(&input, &smallest) ||
            !GetLengthPrefixed(&input, &largest)) {
          return Status::Corruption("version edit", "bad added file");
        }
        f.info.smallest = smallest;
        f.info.largest = largest;
        added.push_back(std::move(f));
        break;
      }
      case kTagRemovedFile: {
        RemovedFile f{};
        if (!GetLevel(&input, &f.level) || !GetVarint64(&input, &f.number)) {
          return Status::Corruption("version edit", "bad removed file");
        }
        removed.push_back(f);
        break;
      }
      case kTagLogNumber:
        if (!GetCounter(&input, &log_number)) {
          return Status::Corruption("version edit", "bad log number");
        }
        break;
      case kTagNextFileNumber:
        if (!GetCounter(&input, &next_file_number)) {
          return Status::Corruption("version edit", "bad next file number");
        }
        break;
      case kTagLastSequence:
        if (!GetCounter(&input, &last_sequence)) {
          return Status::Corruption("version edit", "bad last sequence");
        }
        break;
      default:
        return Status::Corruption("version edit", "unknown tag " + std::to_string(tag));
    }
  }
  return Status::OK();
}

}  // namespace lsmkv
