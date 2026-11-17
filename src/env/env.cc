#include "env/env.h"

namespace lsmkv {

// Out-of-line destructors anchor each class's vtable in this translation unit.
WritableFile::~WritableFile() = default;
ReadableFile::~ReadableFile() = default;
FileLock::~FileLock() = default;
Directory::~Directory() = default;
Env::~Env() = default;

Status CheckFileName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos) {
    return Status::InvalidArgument("bad file name", name);
  }
  return Status::OK();
}

}  // namespace lsmkv
