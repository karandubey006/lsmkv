#pragma once

#include <memory>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"

namespace lsmkv {

// The user's view of a stream of internal keys at a snapshot: for each user
// key, the newest version with sequence <= snapshot, unless that version is a
// deletion, in which case the key is skipped. key() is the user key.
//
// `pin` keeps whatever `internal` reads from alive; it is released after
// `internal` is destroyed.
std::unique_ptr<Iterator> NewDBIterator(std::shared_ptr<const void> pin,
                                        std::unique_ptr<Iterator> internal,
                                        SequenceNumber snapshot);

}  // namespace lsmkv
