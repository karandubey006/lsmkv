#include "table/block_writer.h"

#include <algorithm>
#include <cassert>
#include <limits>

#include "db/dbformat.h"
#include "util/coding.h"

namespace lsmkv {

BlockWriter::BlockWriter(int restart_interval) : restart_interval_(restart_interval) {
  assert(restart_interval_ >= 1);
  restarts_.push_back(0);
}

void BlockWriter::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(key.size() >= kTrailerSize);
  assert(empty() || CompareInternalKeys(key, last_key_) > 0);
  assert(key.size() <= std::numeric_limits<uint32_t>::max());
  assert(value.size() <= std::numeric_limits<uint32_t>::max());

  size_t shared = 0;
  if (since_restart_ == restart_interval_) {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    since_restart_ = 0;
  } else {
    const size_t max_shared = std::min(key.size(), last_key_.size());
    while (shared < max_shared && key[shared] == last_key_[shared]) ++shared;
  }
  const std::string_view suffix = key.substr(shared);

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(suffix.size()));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(suffix);
  buffer_.append(value);
  assert(buffer_.size() <= std::numeric_limits<uint32_t>::max());

  last_key_.resize(shared);
  last_key_.append(suffix);
  ++since_restart_;
  ++num_entries_;
}

std::string_view BlockWriter::Finish() {
  if (!finished_) {
    for (uint32_t offset : restarts_) PutFixed32(&buffer_, offset);
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
    finished_ = true;
  }
  return buffer_;
}

void BlockWriter::Reset() {
  buffer_.clear();
  restarts_.assign(1, 0);
  last_key_.clear();
  num_entries_ = 0;
  since_restart_ = 0;
  finished_ = false;
}

size_t BlockWriter::EstimatedSize() const {
  if (finished_) return buffer_.size();
  return buffer_.size() + 4 * restarts_.size() + 4;
}

}  // namespace lsmkv
