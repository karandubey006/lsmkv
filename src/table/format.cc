#include "table/format.h"

#include <cassert>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset);
  PutVarint64(dst, size);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  std::string_view in = *input;
  uint64_t off;
  uint64_t sz;
  if (!GetVarint64(&in, &off) || !GetVarint64(&in, &sz)) {
    return Status::Corruption("malformed block handle");
  }
  offset = off;
  size = sz;
  *input = in;
  return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
  [[maybe_unused]] const size_t start = dst->size();
  PutFixed64(dst, index.offset);
  PutFixed64(dst, index.size);
  PutFixed64(dst, filter.offset);
  PutFixed64(dst, filter.size);
  PutFixed32(dst, format_version);
  PutFixed64(dst, kSstMagic);
  assert(dst->size() - start == kFooterSize);
}

Status Footer::DecodeFrom(std::string_view input, Footer* footer) {
  if (input.size() != kFooterSize) return Status::Corruption("SST footer has the wrong size");
  const char* p = input.data();
  if (DecodeFixed64(p + 36) != kSstMagic) return Status::Corruption("not an SST file (bad magic)");
  const uint32_t version = DecodeFixed32(p + 32);
  if (version != kSstFormatVersion) {
    return Status::Corruption("unknown SST format version", std::to_string(version));
  }
  footer->index.offset = DecodeFixed64(p);
  footer->index.size = DecodeFixed64(p + 8);
  footer->filter.offset = DecodeFixed64(p + 16);
  footer->filter.size = DecodeFixed64(p + 24);
  footer->format_version = version;
  return Status::OK();
}

bool BlockFits(const BlockHandle& handle, uint64_t limit) {
  return limit >= kBlockTrailerSize && handle.offset <= limit - kBlockTrailerSize &&
         handle.size <= limit - kBlockTrailerSize - handle.offset;
}

Status ReadBlock(const ReadableFile& file, const BlockHandle& handle, uint64_t limit,
                 std::string* contents) {
  if (!BlockFits(handle, limit)) {
    return Status::Corruption("block handle points past the end of the table");
  }
  const size_t n = static_cast<size_t>(handle.size) + kBlockTrailerSize;
  std::string buf(n, '\0');
  std::string_view result;
  Status s = file.ReadAt(handle.offset, n, &result, buf.data());
  if (!s.ok()) return s;
  if (result.size() != n) return Status::Corruption("truncated block");

  const uint32_t stored = DecodeFixed32(buf.data() + handle.size);
  buf.resize(static_cast<size_t>(handle.size));
  if (Crc32c(buf) != stored) return Status::Corruption("block checksum mismatch");
  *contents = std::move(buf);
  return Status::OK();
}

}  // namespace lsmkv
