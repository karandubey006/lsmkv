#pragma once

#include <memory>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Opens the LSM-tree engine: memtables backed by write-ahead logs, flushed to
// SSTs recorded in a MANIFEST.
Status OpenLsmDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr);

// Test-only. Seals the active memtable if it holds anything and waits until
// no flush is pending. Returns the flush error, if any.
// REQUIRES: db was opened by OpenLsmDB.
Status FlushForTesting(DB& db);

}  // namespace lsmkv
