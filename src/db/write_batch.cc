#include "lsmkv/write_batch.h"

#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

// Record tags in the serialized batch. Phase 1 decides the internal key's
// value-type encoding; these may be unified with it then.
enum class Tag : uint8_t {
  kDelete = 0,
  kPut = 1,
};

void SetCount(std::string* rep, uint32_t n) { EncodeFixed32(rep->data(), n); }

}  // namespace

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeaderSize);  // count = 0
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data()); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(Tag::kPut));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(Tag::kDelete));
  PutLengthPrefixed(&rep_, key);
}

Status WriteBatch::Iterate(Handler& handler) const {
  std::string_view input(rep_);
  if (input.size() < WriteBatchInternal::kHeaderSize) {
    return Status::Corruption("write batch", "shorter than header");
  }
  const uint32_t expected = DecodeFixed32(input.data());
  input.remove_prefix(WriteBatchInternal::kHeaderSize);

  uint32_t found = 0;
  std::string_view key, value;
  while (!input.empty()) {
    const auto tag = static_cast<Tag>(input.front());
    input.remove_prefix(1);
    switch (tag) {
      case Tag::kPut:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("write batch", "bad Put record");
        }
        handler.Put(key, value);
        break;
      case Tag::kDelete:
        if (!GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("write batch", "bad Delete record");
        }
        handler.Delete(key);
        break;
      default:
        return Status::Corruption("write batch", "unknown record tag");
    }
    ++found;
  }
  if (found != expected) {
    return Status::Corruption("write batch", "record count does not match header");
  }
  return Status::OK();
}

Status WriteBatchInternal::SetContents(WriteBatch* batch, std::string_view contents) {
  if (contents.size() < kHeaderSize) {
    return Status::Corruption("write batch", "shorter than header");
  }
  batch->rep_.assign(contents);
  return Status::OK();
}

}  // namespace lsmkv
