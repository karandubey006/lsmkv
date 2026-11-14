// POSIX implementation of Env (Linux and macOS).

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>

#include "env/env.h"

namespace lsmkv {
namespace {

constexpr size_t kWritableFileBufferSize = 64 * 1024;

Status PosixError(const std::string& context, int err) {
  std::string msg = std::generic_category().message(err);
  if (err == ENOENT) return Status::NotFound(context, msg);
  return Status::IOError(context, msg);
}

// Forces everything written to fd onto stable storage.
//
// Deliberately never retried on failure: after a failed fsync the kernel may
// already have dropped the dirty pages, so a retry that "succeeds" would lie.
Status SyncFd(int fd, const std::string& name, bool is_dir) {
#if defined(__APPLE__)
  // On macOS fsync() only hands data to the drive, which may hold it in a
  // volatile cache. F_FULLFSYNC also asks the drive to flush that cache.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
  // Some filesystems don't support F_FULLFSYNC; fall back to fsync below.
#endif
#if defined(__linux__)
  // For regular files, fdatasync skips metadata that isn't needed to read the
  // data back (like mtime) but still persists a change in file size.
  int r = is_dir ? ::fsync(fd) : ::fdatasync(fd);
#else
  (void)is_dir;
  int r = ::fsync(fd);
#endif
  if (r == 0) return Status::OK();
  return PosixError(name, errno);
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    size_t total = 0;
    while (total < n) {
      ssize_t r = ::read(fd_, scratch + total, n - total);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      if (r == 0) break;  // end of file
      total += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError(filename_, errno);
    }
    return Status::OK();
  }

 private:
  const int fd_;
  const std::string filename_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    size_t total = 0;
    while (total < n) {
      ssize_t r = ::pread(fd_, scratch + total, n - total,
                          static_cast<off_t>(offset + total));
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      if (r == 0) break;  // end of file
      total += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

 private:
  const int fd_;
  const std::string filename_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {
    buf_.reserve(kWritableFileBufferSize);
  }

  ~PosixWritableFile() override {
    if (fd_ >= 0) static_cast<void>(Close());  // best effort; errors are lost
  }

  Status Append(std::string_view data) override {
    // Fill the buffer as far as possible.
    size_t n = std::min(data.size(), kWritableFileBufferSize - buf_.size());
    buf_.append(data.data(), n);
    data.remove_prefix(n);
    if (data.empty()) return Status::OK();

    // Buffer is full: write it out, then buffer a small remainder or write a
    // large one straight through.
    Status s = FlushBuffer();
    if (!s.ok()) return s;
    if (data.size() < kWritableFileBufferSize) {
      buf_.append(data);
      return Status::OK();
    }
    return WriteUnbuffered(data);
  }

  Status Flush() override { return FlushBuffer(); }

  Status Sync() override {
    Status s = FlushBuffer();
    if (!s.ok()) return s;
    return SyncFd(fd_, filename_, /*is_dir=*/false);
  }

  Status Close() override {
    if (fd_ < 0) return Status::OK();
    Status s = FlushBuffer();
    if (::close(fd_) < 0 && s.ok()) s = PosixError(filename_, errno);
    fd_ = -1;
    return s;
  }

 private:
  Status FlushBuffer() {
    Status s = WriteUnbuffered(buf_);
    buf_.clear();
    return s;
  }

  Status WriteUnbuffered(std::string_view data) {
    while (!data.empty()) {
      ssize_t n = ::write(fd_, data.data(), data.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      data.remove_prefix(static_cast<size_t>(n));  // write() may be partial
    }
    return Status::OK();
  }

  int fd_;
  const std::string filename_;
  std::string buf_;
};

class PosixFileLock final : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }

 private:
  const int fd_;
};

class PosixEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixSequentialFile>(fname, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    int fd = ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    int fd = ::open(fname.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return ::access(fname.c_str(), F_OK) == 0;
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return PosixError(dir, errno);
    while (struct dirent* entry = ::readdir(d)) {
      std::string_view name(entry->d_name);
      if (name == "." || name == "..") continue;
      result->emplace_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat st;
    if (::stat(fname.c_str(), &st) != 0) return PosixError(fname, errno);
    *size = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) return PosixError(fname, errno);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) return PosixError(src, errno);
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) return PosixError(dirname, errno);
    return Status::OK();
  }

  Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) return PosixError(dirname, errno);
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    int fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return PosixError(dirname, errno);
    Status s = SyncFd(fd, dirname, /*is_dir=*/true);
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(fname, errno);
    // flock locks belong to the open file description, so a second open() of
    // the same file, even from this process, conflicts. (fcntl locks would
    // not: they are per process.)
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      int err = errno;
      ::close(fd);
      if (err == EWOULDBLOCK) {
        return Status::IOError("lock " + fname, "already held by another DB instance");
      }
      return PosixError("lock " + fname, err);
    }
    *lock = std::make_unique<PosixFileLock>(fd);
    return Status::OK();
  }

  uint64_t NowMicros() override {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  }
};

}  // namespace

Env* Env::Default() {
  // Intentionally leaked: background threads (from Phase 3 on) may still be
  // using the Env while static destructors run at exit.
  static Env* const env = new PosixEnv;
  return env;
}

}  // namespace lsmkv
