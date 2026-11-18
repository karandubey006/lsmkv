#include "table/sst_reader.h"

#include <cassert>

namespace lsmkv {

// Walks the index block, and for each index entry the data block it points
// to. One data block is held at a time, owned by its block iterator. The
// first error is kept, and the iterator stays invalid from then on.
class SstReader::TableIterator final : public Iterator {
 public:
  explicit TableIterator(const SstReader* table)
      : table_(table), index_iter_(table->index_->NewIterator()) {}

  bool Valid() const override { return block_iter_ != nullptr && block_iter_->Valid(); }

  void SeekToFirst() override {
    if (!status_.ok()) return;
    index_iter_->SeekToFirst();
    LoadBlock();
    if (block_iter_ != nullptr) block_iter_->SeekToFirst();
    SkipExhaustedBlocks();
  }

  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    // The first block whose last key is >= target holds the first entry
    // >= target.
    index_iter_->Seek(target);
    LoadBlock();
    if (block_iter_ != nullptr) block_iter_->Seek(target);
    SkipExhaustedBlocks();
  }

  void Next() override {
    assert(Valid());
    block_iter_->Next();
    SkipExhaustedBlocks();
  }

  std::string_view key() const override {
    assert(Valid());
    return block_iter_->key();
  }

  std::string_view value() const override {
    assert(Valid());
    return block_iter_->value();
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    return index_iter_->status();
  }

 private:
  void Fail(Status s) {
    status_ = std::move(s);
    block_iter_.reset();
  }

  // Replaces block_iter_ with an unpositioned iterator over the block the
  // index iterator is on, or with nothing if it is at the end.
  void LoadBlock() {
    block_iter_.reset();
    if (!index_iter_->Valid()) return;
    std::shared_ptr<const BlockReader> block;
    Status s = table_->ReadBlockForIndexEntry(index_iter_->value(), &block);
    if (!s.ok()) return Fail(std::move(s));
    block_iter_ = BlockReader::NewIterator(std::move(block));
  }

  // While the current block has no more entries, moves to the start of the
  // next one.
  void SkipExhaustedBlocks() {
    while (block_iter_ != nullptr && !block_iter_->Valid()) {
      if (Status s = block_iter_->status(); !s.ok()) return Fail(std::move(s));
      index_iter_->Next();
      LoadBlock();
      if (block_iter_ != nullptr) block_iter_->SeekToFirst();
    }
  }

  const SstReader* const table_;
  const std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> block_iter_;  // null: no block loaded
  Status status_;
};

Status SstReader::Open(std::unique_ptr<ReadableFile> file, uint64_t file_size,
                       std::unique_ptr<SstReader>* out) {
  if (file_size < kFooterSize) return Status::Corruption("file too short to be an SST");

  char scratch[kFooterSize];
  std::string_view footer_bytes;
  Status s = file->ReadAt(file_size - kFooterSize, kFooterSize, &footer_bytes, scratch);
  if (!s.ok()) return s;
  if (footer_bytes.size() != kFooterSize) return Status::Corruption("truncated SST footer");
  Footer footer;
  s = Footer::DecodeFrom(footer_bytes, &footer);
  if (!s.ok()) return s;

  const uint64_t blocks_end = file_size - kFooterSize;
  const bool has_filter = footer.filter.offset != 0 || footer.filter.size != 0;
  if (has_filter && !BlockFits(footer.filter, blocks_end)) {
    return Status::Corruption("filter block handle points past the end of the table");
  }

  std::string index_contents;
  s = ReadBlock(*file, footer.index, blocks_end, &index_contents);
  if (!s.ok()) return s;
  std::unique_ptr<BlockReader> index;
  s = BlockReader::Parse(std::move(index_contents), &index);
  if (!s.ok()) return s;

  out->reset(new SstReader(std::move(file), file_size, footer, std::move(index)));
  return Status::OK();
}

SstReader::SstReader(std::unique_ptr<ReadableFile> file, uint64_t file_size, const Footer& footer,
                     std::unique_ptr<BlockReader> index)
    : file_(std::move(file)), file_size_(file_size), footer_(footer), index_(std::move(index)) {}

Status SstReader::SeekEntry(std::string_view target, bool* found, std::string* key,
                            std::string* value) const {
  *found = false;
  std::unique_ptr<Iterator> index_iter = index_->NewIterator();
  index_iter->Seek(target);
  if (!index_iter->Valid()) return index_iter->status();

  std::shared_ptr<const BlockReader> block;
  Status s = ReadBlockForIndexEntry(index_iter->value(), &block);
  if (!s.ok()) return s;
  std::unique_ptr<Iterator> block_iter = block->NewIterator();
  block_iter->Seek(target);
  if (!block_iter->Valid()) {
    if (!block_iter->status().ok()) return block_iter->status();
    // The index promised an entry >= target in this block.
    return Status::Corruption("data block ends before its index key");
  }
  *found = true;
  key->assign(block_iter->key());
  value->assign(block_iter->value());
  return Status::OK();
}

std::unique_ptr<Iterator> SstReader::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

std::unique_ptr<Iterator> SstReader::NewIndexIterator() const { return index_->NewIterator(); }

Status SstReader::ReadDataBlock(const BlockHandle& handle,
                                std::shared_ptr<const BlockReader>* block) const {
  // Data blocks precede the index block.
  std::string contents;
  Status s = ReadBlock(*file_, handle, footer_.index.offset, &contents);
  if (!s.ok()) return s;
  std::unique_ptr<BlockReader> parsed;
  s = BlockReader::Parse(std::move(contents), &parsed);
  if (!s.ok()) return s;
  *block = std::move(parsed);
  return Status::OK();
}

Status SstReader::ReadBlockForIndexEntry(std::string_view encoded_handle,
                                         std::shared_ptr<const BlockReader>* block) const {
  BlockHandle handle;
  Status s = handle.DecodeFrom(&encoded_handle);
  if (!s.ok()) return s;
  if (!encoded_handle.empty()) return Status::Corruption("trailing bytes after block handle");
  return ReadDataBlock(handle, block);
}

}  // namespace lsmkv
