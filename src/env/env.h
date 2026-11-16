#pragma once

// Env: the single interface through which lsmkv touches the filesystem.
// Nothing outside src/env/ may call open/write/fsync/rename/... directly, so
// tests can substitute an Env that simulates power loss.
//
// Paths are `const std::string&`, not string_view, because every system call
// needs a NUL-terminated string.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv {

// A file read from front to back (WAL and MANIFEST replay).
class SequentialFile {
 public:
  SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  virtual ~SequentialFile();

  // Reads up to n bytes. *result points into scratch (which must hold n bytes)
  // and is shorter than n only at end of file.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;

  // Skips n bytes forward.
  virtual Status Skip(uint64_t n) = 0;
};

// A file read at arbitrary offsets (SSTables). Safe for concurrent use.
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  virtual ~RandomAccessFile();

  // Reads up to n bytes starting at offset. *result points into scratch.
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result,
                      char* scratch) const = 0;
};

// A file written sequentially (WAL, SSTable being built, MANIFEST).
//
// The three levels of "written", each strictly stronger than the last:
//   Append: bytes are in this object's user-space buffer. Lost on process crash.
//   Flush:  bytes are handed to the OS (write()). Survive a process crash, lost
//           on power loss.
//   Sync:   Flush, then force bytes to stable storage (fsync family). Survive
//           power loss.
class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  virtual ~WritableFile();

  virtual Status Append(std::string_view data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// A held lock on a file. Destroying it releases the lock.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  virtual ~FileLock();
};

class Env {
 public:
  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  virtual ~Env();

  // The process-wide POSIX Env. Never null; never destroyed before exit.
  static Env* Default();

  // Open*File returns NotFound if the file does not exist.
  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  // Creates fname, truncating it if it exists.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;
  // Opens fname for appending, creating it if it does not exist.
  virtual Status NewAppendableFile(const std::string& fname,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  // Names (not paths) of the entries in dir, excluding "." and "..".
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  // Atomically replaces target with src (POSIX rename semantics).
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status RemoveDir(const std::string& dirname) = 0;

  // Makes the directory's entries durable: a newly created or renamed file is
  // not guaranteed to survive power loss until its directory is synced.
  virtual Status SyncDir(const std::string& dirname) = 0;

  // Takes an exclusive lock on fname (creating it if needed). Fails with
  // IOError if any other holder, in this process or another, has it.
  virtual Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) = 0;

  // Microseconds from a monotonic clock. Only differences are meaningful.
  virtual uint64_t NowMicros() = 0;
};

}  // namespace lsmkv
