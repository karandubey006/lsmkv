#pragma once

// Write-ahead log file format: a sequence of records and nothing else.
//
//   record := crc:fixed32  length:fixed32  payload[length]
//
// crc is the CRC32C of the 4 length bytes followed by the payload, so a
// damaged length is detected too, and an all-zero header (a zero-filled tail)
// never passes. length is 1 .. 2^32-1; 0 is always invalid.
//
// The format knows nothing about what a payload means. The engine's payload
// is a sequence number followed by a WriteBatch.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include "env/env.h"
#include "lsmkv/status.h"

namespace lsmkv {

inline constexpr size_t kWalHeaderSize = 8;
inline constexpr uint64_t kWalMaxPayload = UINT32_MAX;

// Appends records to a WAL file. Not thread-safe.
//
// Errors are sticky: after an Append or Sync on the file fails, every later
// AddRecord and Sync returns that same error without touching the file,
// because the file may now end in a partial record.
class WalWriter {
 public:
  explicit WalWriter(std::unique_ptr<WritableFile> file);
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Appends one record whose payload is the concatenation of parts, using a
  // single Append on the file. On OK the record is in the kernel: it survives
  // a process crash, but not power loss until Sync() returns OK.
  // InvalidArgument (not sticky) if the payload is empty or too large.
  Status AddRecord(std::initializer_list<std::string_view> parts);

  // Makes every record added so far survive power loss.
  Status Sync();

  // Closes the file without syncing it.
  Status Close();

 private:
  std::unique_ptr<WritableFile> file_;
  std::string record_;  // reused buffer for header + payload
  Status error_;        // first I/O error; once set, returned forever
};

// Parses records from the full contents of a WAL file.
class WalReader {
 public:
  enum class Outcome {
    kRecord,     // *payload holds the next record
    kEnd,        // the file ends exactly where the last good record ended
    kBadRecord,  // the bytes at good_end() are not a valid record
  };

  explicit WalReader(std::string contents);

  // Payloads point into this reader and stay valid for its lifetime. After
  // kEnd or kBadRecord, every later call returns the same outcome.
  Outcome Next(std::string_view* payload);

  // Offset just past the last good record returned.
  uint64_t good_end() const { return pos_; }

  // Why the record at good_end() is bad. Empty unless Next returned kBadRecord.
  std::string_view bad_reason() const { return bad_reason_; }

 private:
  const std::string contents_;
  size_t pos_ = 0;
  std::string_view bad_reason_;
};

}  // namespace lsmkv
