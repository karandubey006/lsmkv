#pragma once

// Phase 0 stand-in engine: a std::map behind the real public API.
//
// It exists so the API, the Env, the build, CI and the differential tester can
// all be finished and proven before any real storage code is written. It keeps
// data in memory only, so everything is lost when the DB is closed. The real
// engine (DBImpl) replaces it piece by piece from Phase 1 on.

#include <memory>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"

namespace lsmkv {

Status OpenMapDB(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr);

}  // namespace lsmkv
