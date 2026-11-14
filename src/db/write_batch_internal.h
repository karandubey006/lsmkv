#pragma once

// Engine-internal access to WriteBatch's serialized form. Not part of the
// public API: callers only ever build batches with Put/Delete.

#include <cstddef>
#include <string_view>

#include "lsmkv/status.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

class WriteBatchInternal {
 public:
  static constexpr size_t kHeaderSize = 4;  // count:fixed32

  static std::string_view Contents(const WriteBatch& batch) { return batch.rep_; }

  // Replaces the batch with raw serialized bytes (e.g. read back from the WAL).
  // Returns Corruption if they are shorter than the header; the records
  // themselves are only validated by Iterate().
  static Status SetContents(WriteBatch* batch, std::string_view contents);
};

}  // namespace lsmkv
