#include "env/env.h"

namespace lsmkv {

// Out-of-line destructors anchor each class's vtable in this translation unit.
SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
FileLock::~FileLock() = default;
Env::~Env() = default;

}  // namespace lsmkv
