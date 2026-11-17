#pragma once

#include <memory>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Opens the LSM-tree engine: memtables backed by write-ahead logs.
Status OpenLsmDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr);

}  // namespace lsmkv
