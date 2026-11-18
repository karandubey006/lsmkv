#include "table/sst_writer.h"

#include <cassert>

#include "db/dbformat.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {
namespace {

// A block buffer that grew past this (a huge value) is released after use.
constexpr size_t kMaxRetainedBuffer = 1 << 20;

}  // namespace

SstWriter::SstWriter(const SstWriterOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.restart_interval),
      index_block_(/*restart_interval=*/1) {}

Status SstWriter::Add(std::string_view internal_key, std::string_view value) {
  assert(!closed_);
  assert(num_entries_ == 0 || CompareInternalKeys(internal_key, last_key_) > 0);
  if (!status_.ok()) return status_;

  if (num_entries_ == 0) smallest_key_.assign(internal_key);
  last_key_.assign(internal_key);
  ++num_entries_;
  data_block_.Add(internal_key, value);
  if (data_block_.EstimatedSize() >= options_.block_size) FlushDataBlock();
  return status_;
}

void SstWriter::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) return;
  BlockHandle handle;
  WriteBlock(data_block_.Finish(), &handle);
  data_block_.Reset();
  if (!status_.ok()) return;
  ++num_data_blocks_;

  // The index key is the block's own last key, which is last_key_ here
  // because blocks are only flushed right after an Add or in Finish.
  handle_buffer_.clear();
  handle.EncodeTo(&handle_buffer_);
  index_block_.Add(last_key_, handle_buffer_);
}

void SstWriter::WriteBlock(std::string_view contents, BlockHandle* handle) {
  block_buffer_.assign(contents);
  PutFixed32(&block_buffer_, Crc32c(contents));
  status_ = file_->Append(block_buffer_);
  if (status_.ok()) {
    handle->offset = offset_;
    handle->size = contents.size();
    offset_ += block_buffer_.size();
  }
  if (block_buffer_.capacity() > kMaxRetainedBuffer) std::string().swap(block_buffer_);
}

Status SstWriter::Finish() {
  assert(!closed_);
  closed_ = true;
  FlushDataBlock();
  if (!status_.ok()) return status_;

  Footer footer;  // no filter block yet: its handle stays {0, 0}
  WriteBlock(index_block_.Finish(), &footer.index);
  if (!status_.ok()) return status_;

  std::string encoded;
  footer.EncodeTo(&encoded);
  status_ = file_->Append(encoded);
  if (status_.ok()) offset_ += encoded.size();
  return status_;
}

void SstWriter::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace lsmkv
