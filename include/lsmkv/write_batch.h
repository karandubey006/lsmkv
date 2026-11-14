#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

// An ordered list of Puts and Deletes applied atomically by DB::Write: after
// any crash, either every operation in the batch is present or none is.
// Later operations in a batch win over earlier ones on the same key.
//
// The batch is stored already serialized, so a later phase can append it to
// the write-ahead log as a single record without re-encoding:
//
//   rep := count:fixed32  record*
//   record := kPut    key:varstring value:varstring
//           | kDelete key:varstring
//   varstring := length:varint32 bytes[length]
//
// Phase 2 will extend the header with a sequence number.
class WriteBatch {
 public:
  // Callback interface for Iterate().
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  WriteBatch();
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;
  WriteBatch(WriteBatch&&) noexcept = default;
  WriteBatch& operator=(WriteBatch&&) noexcept = default;
  ~WriteBatch() = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  // Number of operations in the batch.
  uint32_t Count() const;

  // Size of the serialized batch in bytes.
  size_t ApproximateSize() const { return rep_.size(); }

  // Calls handler for each operation in order. Returns Corruption if the
  // serialized form is malformed (which can only happen for a batch decoded
  // from bytes, e.g. a WAL record). Operations before the malformed point
  // have already been passed to the handler, so a caller that needs
  // atomicity must stage them and apply only on OK.
  Status Iterate(Handler& handler) const;

 private:
  friend class WriteBatchInternal;
  std::string rep_;
};

}  // namespace lsmkv
