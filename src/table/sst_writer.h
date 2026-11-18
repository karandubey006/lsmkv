#pragma once

// Writes an SSTable (format in table/format.h) to a file, front to back.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "env/env.h"
#include "lsmkv/status.h"
#include "table/block_writer.h"
#include "table/format.h"

namespace lsmkv {

struct SstWriterOptions {
  // A data block is closed once its encoded size reaches this many bytes, so
  // blocks can exceed it by up to one entry.
  size_t block_size = 4096;

  // Entries between restart points in data blocks. The index block always
  // uses 1.
  int restart_interval = 16;
};

// Not thread-safe.
//
// Usage: Add() every entry in order, then Finish() (or Abandon() to give up).
// The caller syncs and closes the file afterwards; SstWriter never does.
//
// Errors are sticky: after an Append on the file fails, every later Add and
// Finish returns that error without touching the file.
class SstWriter {
 public:
  // file is not owned and must outlive the SstWriter.
  SstWriter(const SstWriterOptions& options, WritableFile* file);
  SstWriter(const SstWriter&) = delete;
  SstWriter& operator=(const SstWriter&) = delete;

  // REQUIRES: Finish() and Abandon() not called; internal_key is a
  // well-formed internal key greater than every key added before it.
  Status Add(std::string_view internal_key, std::string_view value);

  // Writes the last data block, the index block and the footer. A table
  // with no entries is valid. REQUIRES: Finish() and Abandon() not called.
  Status Finish();

  // Stops building; the file holds an incomplete table the caller should
  // delete. Nothing more is written.
  void Abandon();

  Status status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  uint64_t NumDataBlocks() const { return num_data_blocks_; }
  // Bytes appended to the file so far; the file size once Finish() returns OK.
  uint64_t FileSize() const { return offset_; }
  // Empty while NumEntries() == 0.
  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return last_key_; }

 private:
  // Writes the pending data block and adds its index entry.
  void FlushDataBlock();
  // Appends contents and their CRC trailer; sets *handle to their location.
  void WriteBlock(std::string_view contents, BlockHandle* handle);

  const SstWriterOptions options_;
  WritableFile* const file_;
  BlockWriter data_block_;
  BlockWriter index_block_;
  std::string block_buffer_;  // contents + trailer, written with one Append
  std::string handle_buffer_;
  std::string smallest_key_;
  std::string last_key_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  uint64_t num_data_blocks_ = 0;
  Status status_;
  bool closed_ = false;
};

}  // namespace lsmkv
