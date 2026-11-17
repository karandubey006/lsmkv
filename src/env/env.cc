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

Status ReadEntireFile(Directory& dir, std::string_view name, std::string* contents) {
  std::unique_ptr<ReadableFile> file;
  Status s = dir.Open(name, &file);
  if (!s.ok()) return s;
  uint64_t size = 0;
  s = file->Size(&size);
  if (!s.ok()) return s;
  contents->resize(size);
  std::string_view got;
  s = file->ReadAt(0, contents->size(), &got, contents->data());
  if (!s.ok()) return s;
  contents->resize(got.size());
  return Status::OK();
}

}  // namespace lsmkv
