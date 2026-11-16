#include "db/dbformat.h"

#include <cassert>

#include "util/coding.h"

namespace lsmkv {

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber sequence,
                       ValueType type) {
  assert(sequence <= kMaxSequenceNumber);
  dst->append(user_key);
  PutFixed64(dst, PackTrailer(sequence, type));
}

std::string SeekKey(std::string_view user_key, SequenceNumber snapshot) {
  // kValue is the largest type, so (snapshot, kValue) is the largest trailer
  // an entry at `snapshot` can have.
  std::string key;
  key.reserve(user_key.size() + kTrailerSize);
  AppendInternalKey(&key, user_key, snapshot, ValueType::kValue);
  return key;
}

uint64_t TrailerOf(std::string_view internal_key) {
  assert(internal_key.size() >= kTrailerSize);
  return DecodeFixed64(internal_key.data() + internal_key.size() - kTrailerSize);
}

std::optional<InternalKeyParts> DecodeInternalKey(std::string_view internal_key) {
  if (internal_key.size() < kTrailerSize) return std::nullopt;
  const uint64_t trailer = TrailerOf(internal_key);
  const uint8_t type = trailer & 0xFF;
  if (type > static_cast<uint8_t>(ValueType::kValue)) return std::nullopt;
  return InternalKeyParts{UserKeyOf(internal_key), trailer >> 8, static_cast<ValueType>(type)};
}

int CompareInternalKeys(std::string_view a, std::string_view b) {
  if (const int r = UserKeyOf(a).compare(UserKeyOf(b)); r != 0) return r;
  const uint64_t ta = TrailerOf(a);
  const uint64_t tb = TrailerOf(b);
  return ta > tb ? -1 : (ta < tb ? 1 : 0);
}

}  // namespace lsmkv
