#include "wal/wal.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {
namespace {

// A record buffer that grew past this is released after use, so one huge
// batch doesn't pin its size in memory for the life of the WAL.
constexpr size_t kMaxRetainedBuffer = 1 << 20;

uint32_t RecordCrc(const char* length_bytes, std::string_view payload) {
  return Crc32c(payload, Crc32c(std::string_view(length_bytes, 4)));
}

}  // namespace

WalWriter::WalWriter(std::unique_ptr<WritableFile> file) : file_(std::move(file)) {}

Status WalWriter::AddRecord(std::initializer_list<std::string_view> parts) {
  if (!error_.ok()) return error_;

  uint64_t length = 0;
  for (std::string_view part : parts) length += part.size();
  if (length == 0 || length > kWalMaxPayload) {
    return Status::InvalidArgument("WAL record payload size out of range");
  }

  record_.resize(kWalHeaderSize);
  for (std::string_view part : parts) record_.append(part);
  char* header = record_.data();
  EncodeFixed32(header + 4, static_cast<uint32_t>(length));
  const std::string_view payload = std::string_view(record_).substr(kWalHeaderSize);
  EncodeFixed32(header, RecordCrc(header + 4, payload));

  error_ = file_->Append(record_);
  if (record_.capacity() > kMaxRetainedBuffer) std::string().swap(record_);
  return error_;
}

Status WalWriter::Sync() {
  if (!error_.ok()) return error_;
  error_ = file_->Sync();
  return error_;
}

Status WalWriter::Close() { return file_->Close(); }

WalReader::WalReader(std::string contents) : contents_(std::move(contents)) {}

WalReader::Outcome WalReader::Next(std::string_view* payload) {
  if (!bad_reason_.empty()) return Outcome::kBadRecord;
  const size_t remaining = contents_.size() - pos_;
  if (remaining == 0) return Outcome::kEnd;

  auto bad = [this](std::string_view reason) {
    bad_reason_ = reason;
    return Outcome::kBadRecord;
  };
  if (remaining < kWalHeaderSize) return bad("truncated header");

  const char* header = contents_.data() + pos_;
  const uint32_t stored_crc = DecodeFixed32(header);
  const uint32_t length = DecodeFixed32(header + 4);
  if (length == 0) return bad("zero length");
  // Checked before the CRC so a garbage length never reads past the end.
  if (length > remaining - kWalHeaderSize) return bad("truncated payload");

  const std::string_view body(header + kWalHeaderSize, length);
  if (RecordCrc(header + 4, body) != stored_crc) return bad("checksum mismatch");

  *payload = body;
  pos_ += kWalHeaderSize + length;
  return Outcome::kRecord;
}

}  // namespace lsmkv
