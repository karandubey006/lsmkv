#pragma once

// Encoded WriteBatch, and the WAL payload that carries one.
//
//   batch       := count:fixed32  op*
//   op          := kValue    key:varstring value:varstring    (a Put)
//                | kDeletion key:varstring                    (a Delete)
//   varstring   := length:varint32 bytes[length]
//
//   wal payload := first_seq:fixed64  batch
//
// The op type byte is the ValueType (dbformat.h), so it passes straight to
// the memtable. The batch's ops take sequence numbers first_seq,
// first_seq + 1, ..., first_seq + count - 1, in order.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "lsmkv/status.h"

namespace lsmkv {

inline constexpr size_t kBatchHeaderSize = 4;    // count
inline constexpr size_t kWalPayloadHeaderSize = 8;  // first_seq

struct BatchOp {
  ValueType type;
  std::string_view key;
  std::string_view value;  // empty for a deletion
};

// Reads the operations of an encoded batch, front to back.
//
// Next() can return ops and then hit malformed bytes, so a caller that must
// apply a batch atomically calls ValidateBatch() first.
class BatchCursor {
 public:
  explicit BatchCursor(std::string_view batch);

  // Fills *op with the next operation and returns true. Returns false at the
  // end of the batch or at the first malformed byte; status() says which.
  // Views in *op point into the batch.
  bool Next(BatchOp* op);

  // After Next() returns false: OK at a clean end, Corruption if the batch is
  // malformed or holds a different number of ops than its header says.
  const Status& status() const { return status_; }

  // The op count from the header (0 if the header itself is missing).
  uint32_t count() const { return count_; }

 private:
  bool Fail(std::string_view why);

  std::string_view rest_;
  uint32_t count_ = 0;
  uint32_t seen_ = 0;
  bool done_ = false;
  Status status_;
};

// Corruption unless every byte of batch is well formed and the op count
// matches the header.
Status ValidateBatch(std::string_view batch);

// The bytes that go in front of a batch in its WAL record: the WAL writer
// appends {EncodeWalPayloadHeader(seq), batch.Encoded()} as one record.
std::string EncodeWalPayloadHeader(uint64_t first_seq);

// Splits a WAL payload. Corruption if it's too short to hold the header and
// a batch header. Does not validate the batch's ops.
Status DecodeWalPayload(std::string_view payload, uint64_t* first_seq, std::string_view* batch);

}  // namespace lsmkv
