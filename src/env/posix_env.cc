// POSIX implementation of Env (Linux and macOS).

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <system_error>

#include "env/env.h"

namespace lsmkv {
namespace {

// macOS rejects single read/write calls over INT_MAX bytes; stay well below.
constexpr size_t kMaxIoChunk = size_t{1} << 30;

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

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {}

  ~PosixWritableFile() override { static_cast<void>(Close()); }

  Status Append(std::string_view data) override {
    while (!data.empty()) {
      ssize_t n = ::write(fd_, data.data(), std::min(data.size(), kMaxIoChunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        return PosixError(filename_, errno);
      }
      data.remove_prefix(static_cast<size_t>(n));  // write() may be partial
    }
    return Status::OK();
  }

  Status Sync() override { return SyncFd(fd_, filename_, /*is_dir=*/false); }

  Status Close() override {
    if (fd_ < 0) return Status::OK();
    int r = ::close(fd_);
    fd_ = -1;
    if (r < 0) return PosixError(filename_, errno);
    return Status::OK();
  }

 private:
  int fd_;
  const std::string filename_;
};

class PosixReadableFile final : public ReadableFile {
 public:
  PosixReadableFile(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {}
  ~PosixReadableFile() override { ::close(fd_); }

  Status ReadAt(uint64_t offset, size_t n, std::string_view* result,
                char* scratch) const override {
    size_t total = 0;
    while (total < n) {
      ssize_t r = ::pread(fd_, scratch + total, std::min(n - total, kMaxIoChunk),
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

  Status Size(uint64_t* size) const override {
    struct stat st;
    if (::fstat(fd_, &st) != 0) return PosixError(filename_, errno);
    *size = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }

 private:
  const int fd_;
  const std::string filename_;
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

// Holds the directory's fd open for its whole lifetime. Every operation names
// files relative to that fd (openat, renameat, unlinkat), so the directory
// that gets synced is exactly the one whose entries changed.
class PosixDirectory final : public Directory {
 public:
  PosixDirectory(std::string path, int fd) : fd_(fd), path_(std::move(path)) {}
  ~PosixDirectory() override { ::close(fd_); }

  Status Create(std::string_view name, std::unique_ptr<WritableFile>* file) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    const std::string n(name);
    int fd = ::openat(fd_, n.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(Describe(n), errno);
    *file = std::make_unique<PosixWritableFile>(Describe(n), fd);
    return Status::OK();
  }

  Status Open(std::string_view name, std::unique_ptr<ReadableFile>* file) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    const std::string n(name);
    int fd = ::openat(fd_, n.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(Describe(n), errno);
    *file = std::make_unique<PosixReadableFile>(Describe(n), fd);
    return Status::OK();
  }

  Status Truncate(std::string_view name, uint64_t size) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    const std::string n(name);
    int fd = ::openat(fd_, n.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(Describe(n), errno);
    struct stat st;
    if (::fstat(fd, &st) != 0) {
      s = PosixError(Describe(n), errno);
    } else if (size > static_cast<uint64_t>(st.st_size)) {
      s = Status::InvalidArgument(Describe(n), "truncate would grow the file");
    } else if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
      s = PosixError(Describe(n), errno);
    } else {
      s = SyncFd(fd, Describe(n), /*is_dir=*/false);
    }
    ::close(fd);
    return s;
  }

  Status Rename(std::string_view from, std::string_view to) override {
    Status s = CheckFileName(from);
    if (s.ok()) s = CheckFileName(to);
    if (!s.ok()) return s;
    const std::string f(from), t(to);
    if (::renameat(fd_, f.c_str(), fd_, t.c_str()) != 0) return PosixError(Describe(f), errno);
    return Status::OK();
  }

  Status Remove(std::string_view name) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    const std::string n(name);
    if (::unlinkat(fd_, n.c_str(), 0) != 0) return PosixError(Describe(n), errno);
    return Status::OK();
  }

  Status List(std::vector<std::string>* names) override {
    names->clear();
    // A fresh open of "." rather than a dup of fd_: a dup would share fd_'s
    // read position, leaving the next List() starting at the end.
    int list_fd = ::openat(fd_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (list_fd < 0) return PosixError(path_, errno);
    DIR* d = ::fdopendir(list_fd);
    if (d == nullptr) {
      int err = errno;
      ::close(list_fd);
      return PosixError(path_, err);
    }
    Status s;
    while (true) {
      errno = 0;
      struct dirent* entry = ::readdir(d);
      if (entry == nullptr) {
        if (errno != 0) s = PosixError(path_, errno);
        break;
      }
      std::string_view n(entry->d_name);
      if (n != "." && n != "..") names->emplace_back(n);
    }
    ::closedir(d);  // also closes list_fd
    return s;
  }

  Status Lock(std::string_view name, std::unique_ptr<FileLock>* lock) override {
    Status s = CheckFileName(name);
    if (!s.ok()) return s;
    const std::string n(name);
    int fd = ::openat(fd_, n.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(Describe(n), errno);
    // flock locks belong to the open file description, so a second open() of
    // the same file, even from this process, conflicts. (fcntl locks would
    // not: they are per process.)
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      int err = errno;
      ::close(fd);
      if (err == EWOULDBLOCK) {
        return Status::IOError("lock " + Describe(n), "already held");
      }
      return PosixError("lock " + Describe(n), err);
    }
    *lock = std::make_unique<PosixFileLock>(fd);
    return Status::OK();
  }

  Status Sync() override { return SyncFd(fd_, path_, /*is_dir=*/true); }

  const std::string& path() const override { return path_; }

 private:
  std::string Describe(const std::string& name) const { return path_ + "/" + name; }

  const int fd_;
  const std::string path_;
};

std::string ParentOf(const std::string& path) {
  std::filesystem::path p(path);
  if (!p.has_filename()) p = p.parent_path();  // "a/b/" names "a/b"
  std::filesystem::path parent = p.parent_path();
  return parent.empty() ? "." : parent.string();
}

class PosixEnv final : public Env {
 public:
  Status CreateDirectory(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) != 0) return PosixError(path, errno);
    // The new directory's own name lives in its parent.
    const std::string parent = ParentOf(path);
    int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return PosixError(parent, errno);
    Status s = SyncFd(fd, parent, /*is_dir=*/true);
    ::close(fd);
    return s;
  }

  Status OpenDirectory(const std::string& path, std::unique_ptr<Directory>* dir) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    *dir = std::make_unique<PosixDirectory>(path, fd);
    return Status::OK();
  }
};

}  // namespace

Env* Env::Posix() {
  // Intentionally leaked: background threads may still use the Env while
  // static destructors run at exit.
  static Env* const env = new PosixEnv;
  return env;
}

}  // namespace lsmkv
