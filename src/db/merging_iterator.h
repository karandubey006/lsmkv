#pragma once

#include <memory>
#include <vector>

#include "lsmkv/iterator.h"

namespace lsmkv {

// Merges iterators over internal keys into one stream in internal-key order.
// Each child must yield internal keys in that order. No two children may hold
// the same internal key (sequence numbers are unique, so they never do).
// key(), value() and Seek() all deal in internal keys.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

}  // namespace lsmkv
