#include "table/block_reader.h"

#include <cassert>
#include <limits>

#include "db/dbformat.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

// Decodes an entry header from [p, limit). Returns a pointer to the key
// suffix, or nullptr if the header is malformed or the suffix and value
// don't fit before limit.
const char* DecodeEntryHeader(const char* p, const char* limit, uint32_t* shared,
                              uint32_t* unshared, uint32_t* value_len) {
  if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
  if ((p = GetVarint32Ptr(p, limit, unshared)) == nullptr) return nullptr;
  if ((p = GetVarint32Ptr(p, limit, value_len)) == nullptr) return nullptr;
  const uint64_t remaining = static_cast<uint64_t>(limit - p);
  if (uint64_t{*unshared} + *value_len > remaining) return nullptr;
  return p;
}

bool IsInternalKey(std::string_view key) { return DecodeInternalKey(key).has_value(); }

}  // namespace

Status BlockReader::Parse(std::string contents, std::unique_ptr<BlockReader>* out) {
  const uint64_t size = contents.size();
  if (size < 4 || size > std::numeric_limits<uint32_t>::max()) {
    return Status::Corruption("bad block", "size out of range");
  }
  const uint32_t n = DecodeFixed32(contents.data() + size - 4);
  if (n == 0 || (uint64_t{n} + 1) * 4 > size) {
    return Status::Corruption("bad block", "restart count out of range");
  }
  const auto entries_end = static_cast<uint32_t>(size - (uint64_t{n} + 1) * 4);
  const char* restarts = contents.data() + entries_end;

  if (DecodeFixed32(restarts) != 0) return Status::Corruption("bad block", "first restart not 0");
  if (entries_end == 0 && n != 1) return Status::Corruption("bad block", "restarts in empty block");
  for (uint32_t i = 1; i < n; ++i) {
    const uint32_t prev = DecodeFixed32(restarts + 4 * (i - 1));
    const uint32_t cur = DecodeFixed32(restarts + 4 * i);
    if (cur <= prev || cur >= entries_end) {
      return Status::Corruption("bad block", "restart offsets out of order or range");
    }
  }
  out->reset(new BlockReader(std::move(contents), entries_end, n));
  return Status::OK();
}

BlockReader::BlockReader(std::string contents, uint32_t entries_end, uint32_t num_restarts)
    : contents_(std::move(contents)), entries_end_(entries_end), num_restarts_(num_restarts) {}

uint32_t BlockReader::RestartOffset(uint32_t index) const {
  assert(index < num_restarts_);
  return DecodeFixed32(contents_.data() + entries_end_ + 4 * index);
}

// Positions are tracked as offsets into the block. Once an iterator sees a
// malformed entry its status is Corruption for good and it stays invalid.
class BlockReader::EntryIterator final : public Iterator {
 public:
  EntryIterator(const BlockReader* block, std::shared_ptr<const BlockReader> owner)
      : block_(block), owner_(std::move(owner)) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    if (!status_.ok()) return;
    StartAtRestart(0);
    ParseNext();
  }

  // REQUIRES: target is a well-formed internal key.
  void Seek(std::string_view target) override {
    if (!status_.ok()) return;
    valid_ = false;
    if (block_->entries_end_ == 0) return;

    // Find the first restart whose key is >= target. Every entry before the
    // restart preceding it is < target, so the scan starts there.
    uint32_t lo = 0;
    uint32_t hi = block_->num_restarts_;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      std::string_view mid_key;
      if (!RestartKey(mid, &mid_key)) return;
      if (CompareInternalKeys(mid_key, target) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    StartAtRestart(lo == 0 ? 0 : lo - 1);
    ParseNext();
    while (valid_ && CompareInternalKeys(key_, target) < 0) ParseNext();
  }

  void Next() override {
    assert(valid_);
    ParseNext();
  }

  std::string_view key() const override {
    assert(valid_);
    return key_;
  }

  std::string_view value() const override {
    assert(valid_);
    return value_;
  }

  Status status() const override { return status_; }

 private:
  void Corrupt(std::string_view why) {
    valid_ = false;
    status_ = Status::Corruption("bad block entry", why);
  }

  void StartAtRestart(uint32_t index) {
    key_.clear();
    valid_ = false;
    next_offset_ = block_->RestartOffset(index);
    next_restart_ = index;
  }

  // The full key stored at a restart point, without moving the iterator.
  bool RestartKey(uint32_t index, std::string_view* key) {
    const char* base = block_->contents_.data();
    const char* limit = base + block_->entries_end_;
    uint32_t shared, unshared, value_len;
    const char* p = DecodeEntryHeader(base + block_->RestartOffset(index), limit, &shared,
                                      &unshared, &value_len);
    if (p == nullptr || shared != 0) {
      Corrupt("malformed restart entry");
      return false;
    }
    *key = std::string_view(p, unshared);
    if (!IsInternalKey(*key)) {
      Corrupt("restart key is not an internal key");
      return false;
    }
    return true;
  }

  // Decodes the entry at next_offset_, or marks the iterator invalid at the
  // end of the entries.
  void ParseNext() {
    const uint32_t offset = next_offset_;
    if (offset >= block_->entries_end_) {
      valid_ = false;
      return;
    }

    bool at_restart = false;
    if (next_restart_ < block_->num_restarts_) {
      const uint32_t restart = block_->RestartOffset(next_restart_);
      if (offset == restart) {
        at_restart = true;
        ++next_restart_;
      } else if (offset > restart) {
        Corrupt("restart point inside an entry");
        return;
      }
    }

    const char* base = block_->contents_.data();
    uint32_t shared, unshared, value_len;
    const char* p = DecodeEntryHeader(base + offset, base + block_->entries_end_, &shared,
                                      &unshared, &value_len);
    if (p == nullptr) {
      Corrupt("malformed entry header");
      return;
    }
    if (at_restart ? shared != 0 : shared > key_.size()) {
      Corrupt("bad shared key length");
      return;
    }
    key_.resize(shared);
    key_.append(p, unshared);
    if (!IsInternalKey(key_)) {
      Corrupt("key is not an internal key");
      return;
    }
    value_ = std::string_view(p + unshared, value_len);
    next_offset_ = static_cast<uint32_t>(p + unshared + value_len - base);
    valid_ = true;
  }

  const BlockReader* const block_;
  const std::shared_ptr<const BlockReader> owner_;  // null unless the iterator owns the block

  std::string key_;
  std::string_view value_;  // points into the block
  uint32_t next_offset_ = 0;
  uint32_t next_restart_ = 0;  // index of the first restart point not yet reached
  bool valid_ = false;
  Status status_;
};

std::unique_ptr<Iterator> BlockReader::NewIterator() const {
  return std::make_unique<EntryIterator>(this, nullptr);
}

std::unique_ptr<Iterator> BlockReader::NewIterator(std::shared_ptr<const BlockReader> block) {
  const BlockReader* raw = block.get();
  return std::make_unique<EntryIterator>(raw, std::move(block));
}

}  // namespace lsmkv
