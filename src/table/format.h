#pragma once

// SSTable file format. All fixed-width integers are little-endian.
//
//   file    := data_block*  [filter_block]  index_block  footer
//   block   := contents  crc:fixed32          (crc = CRC32C of contents)
//   footer  := index_offset:fixed64  index_size:fixed64
//              filter_offset:fixed64 filter_size:fixed64
//              format_version:fixed32  magic:fixed64           (44 bytes)
//
// A block handle's size covers the contents only, not the 4-byte CRC that
// follows them. Blocks are stored uncompressed. A filter handle of offset 0,
// size 0 means the table has no filter block.
//
// Block contents (data and index blocks) are described in block_writer.h.
// The index block holds one entry per data block, in file order: the data
// block's last internal key, mapped to its handle encoded as
//
//   handle  := offset:varint64  size:varint64

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "env/env.h"
#include "lsmkv/status.h"

namespace lsmkv {

inline constexpr size_t kBlockTrailerSize = 4;
inline constexpr size_t kFooterSize = 44;
inline constexpr uint32_t kSstFormatVersion = 1;

namespace table_internal {
constexpr uint64_t AsciiToFixed64(const char (&s)[9]) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(s[i]);
  return v;
}
}  // namespace table_internal

// The bytes "lsmkvSST", read as a little-endian fixed64.
inline constexpr uint64_t kSstMagic = table_internal::AsciiToFixed64("lsmkvSST");

// The location of a block's contents within a table file.
struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;

  // Appends the varint encoding.
  void EncodeTo(std::string* dst) const;

  // Consumes a varint-encoded handle from the front of *input. Corruption
  // (leaving *input unchanged) if it is truncated or malformed.
  Status DecodeFrom(std::string_view* input);

  friend bool operator==(const BlockHandle&, const BlockHandle&) = default;
};

struct Footer {
  BlockHandle index;
  BlockHandle filter;  // {0, 0}: no filter block
  uint32_t format_version = kSstFormatVersion;

  // Appends exactly kFooterSize bytes, ending with kSstMagic.
  void EncodeTo(std::string* dst) const;

  // Parses exactly kFooterSize bytes. Corruption if the size or magic is
  // wrong or the format version is not one this build can read. Does not
  // check the handles against the file size.
  static Status DecodeFrom(std::string_view input, Footer* footer);
};

// True if a block at handle, plus its trailer, ends at or before limit.
// Safe against overflow for any handle.
bool BlockFits(const BlockHandle& handle, uint64_t limit);

// Reads the block at handle, verifies its CRC and replaces *contents with
// the block contents (without the trailer). `limit` is the file offset the
// block plus its trailer must end at or before; a handle that extends past
// it is Corruption, checked before anything is read or allocated.
Status ReadBlock(const ReadableFile& file, const BlockHandle& handle, uint64_t limit,
                 std::string* contents);

}  // namespace lsmkv
