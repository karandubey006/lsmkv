#pragma once

// Internal keys tag a user key with the sequence number and type of the write:
//
//   internal_key := user_key | trailer
//   trailer      := fixed64(sequence << 8 | type)
//
// Sorted by user key ascending (byte-wise), then trailer descending, so the
// newest version of a user key comes first.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lsmkv {

using SequenceNumber = uint64_t;

inline constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;
inline constexpr size_t kTrailerSize = 8;

// Persisted (WriteBatch record tags, WAL). Do not renumber.
enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

struct InternalKeyParts {
  std::string_view user_key;
  SequenceNumber sequence;
  ValueType type;
};

inline uint64_t PackTrailer(SequenceNumber sequence, ValueType type) {
  return (sequence << 8) | static_cast<uint8_t>(type);
}

void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber sequence,
                       ValueType type);

// The key to seek to for the newest version of user_key visible at
// `snapshot`. It sorts at or before every entry of user_key with sequence <=
// snapshot, and after every newer one.
std::string SeekKey(std::string_view user_key, SequenceNumber snapshot);

// Returns nullopt if internal_key is too short or has an unknown type.
// The result's user_key points into internal_key.
std::optional<InternalKeyParts> DecodeInternalKey(std::string_view internal_key);

// REQUIRES for the functions below: the arguments are well-formed internal keys.

inline std::string_view UserKeyOf(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - kTrailerSize);
}

uint64_t TrailerOf(std::string_view internal_key);

inline ValueType TypeOf(std::string_view internal_key) {
  return static_cast<ValueType>(TrailerOf(internal_key) & 0xFF);
}

// < 0, 0 or > 0 as a sorts before, equal to, or after b.
int CompareInternalKeys(std::string_view a, std::string_view b);

}  // namespace lsmkv
