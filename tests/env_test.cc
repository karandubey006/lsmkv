#include "env/env.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "test_util.h"

namespace lsmkv {
namespace {

class EnvTest : public ::testing::Test {
 protected:
  std::string Path(const std::string& name) const { return dir_.path() + "/" + name; }

  void WriteFile(const std::string& name, std::string_view contents) {
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(env_->NewWritableFile(Path(name), &f));
    ASSERT_OK(f->Append(contents));
    ASSERT_OK(f->Sync());
    ASSERT_OK(f->Close());
  }

  std::string ReadFile(const std::string& name) {
    std::unique_ptr<SequentialFile> f;
    Status s = env_->NewSequentialFile(Path(name), &f);
    EXPECT_TRUE(s.ok()) << s.ToString();
    if (!s.ok()) return "";
    std::string out;
    char scratch[4096];
    while (true) {
      std::string_view chunk;
      s = f->Read(sizeof(scratch), &chunk, scratch);
      EXPECT_TRUE(s.ok()) << s.ToString();
      if (!s.ok() || chunk.empty()) break;
      out.append(chunk);
    }
    return out;
  }

  Env* env_ = Env::Default();
  test::TempDir dir_;
};

TEST_F(EnvTest, WriteThenReadBack) {
  WriteFile("f", "hello world");
  EXPECT_EQ(ReadFile("f"), "hello world");
  uint64_t size = 0;
  ASSERT_OK(env_->GetFileSize(Path("f"), &size));
  EXPECT_EQ(size, 11u);
}

TEST_F(EnvTest, LargeAppendBypassesBufferCorrectly) {
  // Larger than the 64 KB write buffer, written in uneven pieces.
  std::string expected;
  std::unique_ptr<WritableFile> f;
  ASSERT_OK(env_->NewWritableFile(Path("big"), &f));
  for (int i = 0; i < 50; ++i) {
    std::string piece(static_cast<size_t>(i) * 997 + 1, static_cast<char>('a' + i % 26));
    expected += piece;
    ASSERT_OK(f->Append(piece));
  }
  std::string huge(300 * 1024, 'Z');
  expected += huge;
  ASSERT_OK(f->Append(huge));
  ASSERT_OK(f->Close());
  EXPECT_EQ(ReadFile("big"), expected);
}

TEST_F(EnvTest, NewWritableFileTruncates) {
  WriteFile("f", "long old contents");
  WriteFile("f", "new");
  EXPECT_EQ(ReadFile("f"), "new");
}

TEST_F(EnvTest, AppendableFileAppends) {
  WriteFile("f", "abc");
  std::unique_ptr<WritableFile> f;
  ASSERT_OK(env_->NewAppendableFile(Path("f"), &f));
  ASSERT_OK(f->Append("def"));
  ASSERT_OK(f->Close());
  EXPECT_EQ(ReadFile("f"), "abcdef");
}

TEST_F(EnvTest, FlushMakesDataVisibleBeforeClose) {
  std::unique_ptr<WritableFile> f;
  ASSERT_OK(env_->NewWritableFile(Path("f"), &f));
  ASSERT_OK(f->Append("buffered"));
  ASSERT_OK(f->Flush());
  EXPECT_EQ(ReadFile("f"), "buffered");  // reached the OS without a Close
  ASSERT_OK(f->Close());
}

TEST_F(EnvTest, SequentialSkip) {
  WriteFile("f", "0123456789");
  std::unique_ptr<SequentialFile> f;
  ASSERT_OK(env_->NewSequentialFile(Path("f"), &f));
  ASSERT_OK(f->Skip(4));
  char scratch[3];
  std::string_view out;
  ASSERT_OK(f->Read(3, &out, scratch));
  EXPECT_EQ(out, "456");
}

TEST_F(EnvTest, RandomAccessReads) {
  WriteFile("f", "0123456789");
  std::unique_ptr<RandomAccessFile> f;
  ASSERT_OK(env_->NewRandomAccessFile(Path("f"), &f));
  char scratch[16];
  std::string_view out;
  ASSERT_OK(f->Read(3, 4, &out, scratch));
  EXPECT_EQ(out, "3456");
  ASSERT_OK(f->Read(8, 10, &out, scratch));  // runs past end of file
  EXPECT_EQ(out, "89");
  ASSERT_OK(f->Read(100, 4, &out, scratch));
  EXPECT_EQ(out, "");
}

TEST_F(EnvTest, MissingFileIsNotFound) {
  std::unique_ptr<SequentialFile> s;
  EXPECT_TRUE(env_->NewSequentialFile(Path("nope"), &s).IsNotFound());
  std::unique_ptr<RandomAccessFile> r;
  EXPECT_TRUE(env_->NewRandomAccessFile(Path("nope"), &r).IsNotFound());
  uint64_t size;
  EXPECT_TRUE(env_->GetFileSize(Path("nope"), &size).IsNotFound());
  EXPECT_TRUE(env_->RemoveFile(Path("nope")).IsNotFound());
  EXPECT_FALSE(env_->FileExists(Path("nope")));
}

TEST_F(EnvTest, ChildrenRenameRemove) {
  WriteFile("a", "1");
  WriteFile("b", "2");
  std::vector<std::string> children;
  ASSERT_OK(env_->GetChildren(dir_.path(), &children));
  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, (std::vector<std::string>{"a", "b"}));

  // Rename atomically replaces an existing target.
  ASSERT_OK(env_->RenameFile(Path("a"), Path("b")));
  EXPECT_FALSE(env_->FileExists(Path("a")));
  EXPECT_EQ(ReadFile("b"), "1");
  ASSERT_OK(env_->SyncDir(dir_.path()));

  ASSERT_OK(env_->RemoveFile(Path("b")));
  ASSERT_OK(env_->GetChildren(dir_.path(), &children));
  EXPECT_TRUE(children.empty());
}

TEST_F(EnvTest, CreateAndRemoveDir) {
  ASSERT_OK(env_->CreateDir(Path("sub")));
  EXPECT_TRUE(env_->FileExists(Path("sub")));
  EXPECT_FALSE(env_->CreateDir(Path("sub")).ok());  // already exists
  ASSERT_OK(env_->RemoveDir(Path("sub")));
  EXPECT_FALSE(env_->FileExists(Path("sub")));
}

TEST_F(EnvTest, LockIsExclusiveEvenWithinOneProcess) {
  std::unique_ptr<FileLock> first;
  ASSERT_OK(env_->LockFile(Path("LOCK"), &first));

  std::unique_ptr<FileLock> second;
  Status s = env_->LockFile(Path("LOCK"), &second);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  EXPECT_EQ(second, nullptr);

  first.reset();  // releasing the lock lets the next holder in
  ASSERT_OK(env_->LockFile(Path("LOCK"), &second));
}

TEST_F(EnvTest, NowMicrosIsMonotonic) {
  uint64_t a = env_->NowMicros();
  uint64_t b = env_->NowMicros();
  EXPECT_LE(a, b);
}

}  // namespace
}  // namespace lsmkv
