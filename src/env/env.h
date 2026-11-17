#pragma once

// Env: the single interface through which lsmkv touches the filesystem.
// Nothing outside src/env/ may call open/write/fsync/rename/... directly, so
// tests can substitute an Env that simulates power loss.
//
// Durability model:
//   - A file's contents are durable up to the last successful Sync() on it.
//   - A file's *name* (its creation, rename or removal) is durable only after
//     the Directory that holds it is synced. Every name change therefore goes
//     through a Directory, which is also the object that syncs it.
//
// File names passed to a Directory are single path components: non-empty,
// not "." or "..", and free of '/'. Anything else is InvalidArgument.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv {

// A file opened for appending. Not thread-safe.
class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  virtual ~WritableFile();

  // Writes data at the end of the file. On OK, all of it has been handed to
  // the kernel and survives a process crash. On error, a prefix of it may
  // have been written.
  virtual Status Append(std::string_view data) = 0;

  // Makes every byte appended so far survive power loss.
  virtual Status Sync() = 0;

  // Closes the file without syncing it. Calling it again is a no-op. The
  // destructor closes the file if Close() wasn't called, discarding errors.
  virtual Status Close() = 0;
};

// A file opened for reading. Thread-safe.
class ReadableFile {
 public:
  ReadableFile() = default;
  ReadableFile(const ReadableFile&) = delete;
  ReadableFile& operator=(const ReadableFile&) = delete;
  virtual ~ReadableFile();

  // Reads up to n bytes starting at offset into scratch, which must hold n
  // bytes. *result points into scratch and is shorter than n only when the
  // read reaches the end of the file.
  virtual Status ReadAt(uint64_t offset, size_t n, std::string_view* result,
                        char* scratch) const = 0;

  virtual Status Size(uint64_t* size) const = 0;
};

// An exclusive lock on a file. Destroying it releases the lock.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  virtual ~FileLock();
};

// An open directory. Owns every name change inside it; see the durability
// model above. Thread-safe.
class Directory {
 public:
  Directory() = default;
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  virtual ~Directory();

  // Creates a new, empty file. Fails with IOError if name already exists, so
  // a live file can't be truncated by accident.
  virtual Status Create(std::string_view name, std::unique_ptr<WritableFile>* file) = 0;

  // Opens an existing file for reading. NotFound if it doesn't exist.
  virtual Status Open(std::string_view name, std::unique_ptr<ReadableFile>* file) = 0;

  // Shortens an existing file to size bytes and syncs it. On OK the new size
  // is durable. size must not exceed the current size.
  virtual Status Truncate(std::string_view name, uint64_t size) = 0;

  // Atomically replaces `to` (if it exists) with `from`.
  virtual Status Rename(std::string_view from, std::string_view to) = 0;

  // NotFound if name doesn't exist.
  virtual Status Remove(std::string_view name) = 0;

  // Names of every entry, excluding "." and "..", in no particular order.
  virtual Status List(std::vector<std::string>* names) = 0;

  // Takes an exclusive lock on name, creating the file if needed. Fails with
  // IOError if another holder, in this process or another, has it.
  virtual Status Lock(std::string_view name, std::unique_ptr<FileLock>* lock) = 0;

  // Makes every Create, Rename and Remove done so far survive power loss.
  virtual Status Sync() = 0;

  virtual const std::string& path() const = 0;
};

class Env {
 public:
  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  virtual ~Env();

  // The process-wide POSIX Env. Never null; never destroyed before exit.
  static Env* Posix();

  // Creates a directory. On OK its existence is durable (the parent directory
  // has been synced). Fails with IOError if path already exists.
  virtual Status CreateDirectory(const std::string& path) = 0;

  // Opens an existing directory. NotFound if it doesn't exist.
  virtual Status OpenDirectory(const std::string& path, std::unique_ptr<Directory>* dir) = 0;
};

// Returns InvalidArgument unless name is a single path component.
Status CheckFileName(std::string_view name);

// Replaces *contents with the whole file.
Status ReadEntireFile(Directory& dir, std::string_view name, std::string* contents);

}  // namespace lsmkv
