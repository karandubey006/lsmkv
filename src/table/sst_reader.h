#pragma once

// Reads an SSTable (format in table/format.h).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "env/env.h"
#include "lsmkv/iterator.h"
#include "lsmkv/status.h"
#include "table/block_reader.h"
#include "table/format.h"

namespace lsmkv {

// An open table. The footer and index block are read by Open and kept in
// memory; data blocks are read from the file on demand, and every block read
// verifies its CRC.
//
// Thread-safe: all methods are const and may be called concurrently.
class SstReader {
 public:
  // Reads the footer and index block of a table file_size bytes long.
  // Corruption if the file is too short, isn't an SST, has an unknown format
  // version, or its footer or index is damaged.
  static Status Open(std::unique_ptr<ReadableFile> file, uint64_t file_size,
                     std::unique_ptr<SstReader>* out);

  SstReader(const SstReader&) = delete;
  SstReader& operator=(const SstReader&) = delete;

  // Finds the first entry whose internal key is >= target, reading at most
  // one data block. Sets *found to whether there is one; if so, copies it
  // into *key and *value. REQUIRES: target is a well-formed internal key.
  Status SeekEntry(std::string_view target, bool* found, std::string* key,
                   std::string* value) const;

  // An iterator over every entry, keyed by internal key. Data blocks are
  // read lazily, one at a time; read and decode errors make it invalid and
  // are reported by status(). It must be destroyed before this SstReader.
  std::unique_ptr<Iterator> NewIterator() const;

  // An iterator over the index block: one entry per data block, in file
  // order, whose key is the block's last internal key and whose value is its
  // encoded BlockHandle. It must be destroyed before this SstReader.
  std::unique_ptr<Iterator> NewIndexIterator() const;

  // Reads and parses the data block at handle.
  Status ReadDataBlock(const BlockHandle& handle, std::shared_ptr<const BlockReader>* block) const;

  const Footer& footer() const { return footer_; }
  uint64_t file_size() const { return file_size_; }

 private:
  SstReader(std::unique_ptr<ReadableFile> file, uint64_t file_size, const Footer& footer,
            std::unique_ptr<BlockReader> index);

  // Reads the data block an index entry points to.
  Status ReadBlockForIndexEntry(std::string_view encoded_handle,
                                std::shared_ptr<const BlockReader>* block) const;

  class TableIterator;

  const std::unique_ptr<ReadableFile> file_;
  const uint64_t file_size_;
  const Footer footer_;
  const std::unique_ptr<BlockReader> index_;
};

}  // namespace lsmkv
