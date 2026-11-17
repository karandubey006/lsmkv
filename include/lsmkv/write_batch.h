#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsmkv {

// An ordered list of Puts and Deletes applied atomically by DB::Write: after
// any crash, either every operation in the batch is present or none is.
// Later operations in a batch win over earlier ones on the same key.
class WriteBatch {
 public:
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

  // The batch's encoded bytes, exactly as they are written to the log. The
  // format is internal to lsmkv; use the size to bound memory or batch size.
  std::string_view Encoded() const { return encoded_; }

 private:
  std::string encoded_;
};

}  // namespace lsmkv
