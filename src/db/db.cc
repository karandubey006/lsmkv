#include "lsmkv/db.h"

#include "db/lsm_db.h"

namespace lsmkv {

Status DB::Open(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr) {
  return OpenLsmDB(options, name, dbptr);
}

Status DB::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, batch);
}

Status DB::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, batch);
}

}  // namespace lsmkv
