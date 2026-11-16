#include "lsmkv/write_batch.h"

#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

// Record tags are ValueTypes, so they pass straight through to the memtable.
using Tag = ValueType;

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
  rep_.push_back(static_cast<char>(Tag::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(Tag::kDeletion));
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
      case Tag::kValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("write batch", "bad Put record");
        }
        handler.Put(key, value);
        break;
      case Tag::kDeletion:
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
