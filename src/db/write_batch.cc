#include "lsmkv/write_batch.h"

#include "db/batch_format.h"
#include "util/coding.h"

namespace lsmkv {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  encoded_.assign(kBatchHeaderSize, '\0');  // count = 0
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(encoded_.data()); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  EncodeFixed32(encoded_.data(), Count() + 1);
  encoded_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&encoded_, key);
  PutLengthPrefixed(&encoded_, value);
}

void WriteBatch::Delete(std::string_view key) {
  EncodeFixed32(encoded_.data(), Count() + 1);
  encoded_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&encoded_, key);
}

BatchCursor::BatchCursor(std::string_view batch) : rest_(batch) {
  if (rest_.size() < kBatchHeaderSize) {
    Fail("shorter than its header");
    return;
  }
  count_ = DecodeFixed32(rest_.data());
  rest_.remove_prefix(kBatchHeaderSize);
}

bool BatchCursor::Fail(std::string_view why) {
  done_ = true;
  status_ = Status::Corruption("write batch", why);
  return false;
}

bool BatchCursor::Next(BatchOp* op) {
  if (done_) return false;
  if (rest_.empty()) {
    done_ = true;
    if (seen_ != count_) return Fail("op count does not match header");
    return false;
  }

  const auto type = static_cast<ValueType>(rest_.front());
  rest_.remove_prefix(1);
  switch (type) {
    case ValueType::kValue:
      if (!GetLengthPrefixed(&rest_, &op->key) || !GetLengthPrefixed(&rest_, &op->value)) {
        return Fail("truncated put");
      }
      break;
    case ValueType::kDeletion:
      if (!GetLengthPrefixed(&rest_, &op->key)) return Fail("truncated delete");
      op->value = {};
      break;
    default:
      return Fail("unknown op type");
  }
  op->type = type;
  ++seen_;
  return true;
}

Status ValidateBatch(std::string_view batch) {
  BatchCursor cursor(batch);
  BatchOp op;
  while (cursor.Next(&op)) {
  }
  return cursor.status();
}

std::string EncodeWalPayloadHeader(uint64_t first_seq) {
  std::string header(kWalPayloadHeaderSize, '\0');
  EncodeFixed64(header.data(), first_seq);
  return header;
}

Status DecodeWalPayload(std::string_view payload, uint64_t* first_seq, std::string_view* batch) {
  if (payload.size() < kWalPayloadHeaderSize + kBatchHeaderSize) {
    return Status::Corruption("WAL payload", "too short");
  }
  *first_seq = DecodeFixed64(payload.data());
  *batch = payload.substr(kWalPayloadHeaderSize);
  return Status::OK();
}

}  // namespace lsmkv
