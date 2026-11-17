// Contract tests run against every Env implementation, so the fault-injection
// Env is held to the same behaviour as the POSIX one.

#include "env/env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

#include "env/fault_injection_env.h"
#include "test_util.h"

namespace lsmkv {
namespace {

struct EnvFactory {
  const char* name;
  std::function<Env*()> make;  // the Env, or nullptr for Env::Posix()
};

class EnvContractTest : public ::testing::TestWithParam<EnvFactory> {
 protected:
  void SetUp() override {
    owned_.reset(GetParam().make());
    env_ = owned_ ? owned_.get() : Env::Posix();
    root_ = owned_ ? std::string("/fake/db") : tmp_.path() + "/db";
    ASSERT_OK(env_->CreateDirectory(root_));
    ASSERT_OK(env_->OpenDirectory(root_, &dir_));
  }

  void WriteFile(std::string_view name, std::string_view contents) {
    std::unique_ptr<WritableFile> f;
    ASSERT_OK(dir_->Create(name, &f));
    ASSERT_OK(f->Append(contents));
    ASSERT_OK(f->Close());
  }

  std::string ReadFile(std::string_view name) {
    std::unique_ptr<ReadableFile> f;
    Status s = dir_->Open(name, &f);
    EXPECT_TRUE(s.ok()) << s.ToString();
    if (!s.ok()) return "<missing>";
    uint64_t size = 0;
    EXPECT_OK(f->Size(&size));
    std::string scratch(size, '\0');
    std::string_view out;
    EXPECT_OK(f->ReadAt(0, scratch.size(), &out, scratch.data()));
    return std::string(out);
  }

  std::vector<std::string> SortedList() {
    std::vector<std::string> names;
    EXPECT_OK(dir_->List(&names));
    std::sort(names.begin(), names.end());
    return names;
  }

  test::TempDir tmp_;
  std::unique_ptr<Env> owned_;
  Env* env_ = nullptr;
  std::string root_;
  std::unique_ptr<Directory> dir_;
};

TEST_P(EnvContractTest, WriteThenReadBack) {
  std::unique_ptr<WritableFile> f;
  ASSERT_OK(dir_->Create("f", &f));
  ASSERT_OK(f->Append("hello "));
  ASSERT_OK(f->Append("world"));
  ASSERT_OK(f->Sync());
  ASSERT_OK(f->Close());
  ASSERT_OK(f->Close());  // second close is a no-op
  EXPECT_EQ(ReadFile("f"), "hello world");
}

TEST_P(EnvContractTest, AppendIsVisibleToReadersWithoutSyncOrClose) {
  std::unique_ptr<WritableFile> f;
  ASSERT_OK(dir_->Create("f", &f));
  ASSERT_OK(f->Append("in the kernel"));
  EXPECT_EQ(ReadFile("f"), "in the kernel");
}

TEST_P(EnvContractTest, LargeAppend) {
  std::string big(3 * 1024 * 1024 + 17, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31);
  WriteFile("big", big);
  EXPECT_EQ(ReadFile("big"), big);
}

TEST_P(EnvContractTest, ReadAtOffsetsAndEndOfFile) {
  WriteFile("f", "0123456789");
  std::unique_ptr<ReadableFile> f;
  ASSERT_OK(dir_->Open("f", &f));
  char scratch[16];
  std::string_view out;
  ASSERT_OK(f->ReadAt(3, 4, &out, scratch));
  EXPECT_EQ(out, "3456");
  ASSERT_OK(f->ReadAt(8, 10, &out, scratch));  // runs past the end
  EXPECT_EQ(out, "89");
  ASSERT_OK(f->ReadAt(100, 4, &out, scratch));
  EXPECT_EQ(out, "");
  uint64_t size = 0;
  ASSERT_OK(f->Size(&size));
  EXPECT_EQ(size, 10u);
}

TEST_P(EnvContractTest, CreateRefusesExistingName) {
  WriteFile("f", "keep me");
  std::unique_ptr<WritableFile> f;
  EXPECT_TRUE(dir_->Create("f", &f).IsIOError());
  EXPECT_EQ(ReadFile("f"), "keep me");
}

TEST_P(EnvContractTest, MissingFilesAreNotFound) {
  std::unique_ptr<ReadableFile> r;
  EXPECT_TRUE(dir_->Open("nope", &r).IsNotFound());
  EXPECT_TRUE(dir_->Remove("nope").IsNotFound());
  EXPECT_TRUE(dir_->Rename("nope", "x").IsNotFound());
  EXPECT_TRUE(dir_->Truncate("nope", 0).IsNotFound());
}

TEST_P(EnvContractTest, BadNamesAreRejected) {
  std::unique_ptr<WritableFile> f;
  for (std::string_view name : {"", ".", "..", "a/b", "/abs"}) {
    EXPECT_TRUE(dir_->Create(name, &f).IsInvalidArgument()) << name;
  }
  EXPECT_TRUE(dir_->Create(std::string_view("a\0b", 3), &f).IsInvalidArgument());
}

TEST_P(EnvContractTest, ListRenameRemove) {
  WriteFile("a", "1");
  WriteFile("b", "2");
  EXPECT_EQ(SortedList(), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(SortedList(), (std::vector<std::string>{"a", "b"}));  // List is repeatable

  ASSERT_OK(dir_->Rename("a", "b"));  // replaces the existing target
  EXPECT_EQ(SortedList(), (std::vector<std::string>{"b"}));
  EXPECT_EQ(ReadFile("b"), "1");

  ASSERT_OK(dir_->Remove("b"));
  EXPECT_TRUE(SortedList().empty());
  ASSERT_OK(dir_->Sync());
}

TEST_P(EnvContractTest, Truncate) {
  WriteFile("f", "0123456789");
  ASSERT_OK(dir_->Truncate("f", 4));
  EXPECT_EQ(ReadFile("f"), "0123");
  EXPECT_TRUE(dir_->Truncate("f", 5).IsInvalidArgument());  // can't grow
  ASSERT_OK(dir_->Truncate("f", 0));
  EXPECT_EQ(ReadFile("f"), "");
}

TEST_P(EnvContractTest, Directories) {
  std::unique_ptr<Directory> d;
  EXPECT_TRUE(env_->OpenDirectory(root_ + "-missing", &d).IsNotFound());
  EXPECT_TRUE(env_->CreateDirectory(root_).IsIOError());  // already exists
  EXPECT_EQ(dir_->path(), root_);
}

TEST_P(EnvContractTest, LockIsExclusiveEvenWithinOneProcess) {
  std::unique_ptr<FileLock> first;
  ASSERT_OK(dir_->Lock("LOCK", &first));

  // A second handle on the same directory still conflicts.
  std::unique_ptr<Directory> other;
  ASSERT_OK(env_->OpenDirectory(root_, &other));
  std::unique_ptr<FileLock> second;
  EXPECT_TRUE(other->Lock("LOCK", &second).IsIOError());
  EXPECT_EQ(second, nullptr);

  first.reset();  // releasing the lock lets the next holder in
  ASSERT_OK(other->Lock("LOCK", &second));
  EXPECT_EQ(SortedList(), (std::vector<std::string>{"LOCK"}));
}

INSTANTIATE_TEST_SUITE_P(
    AllEnvs, EnvContractTest,
    ::testing::Values(EnvFactory{"Posix", []() -> Env* { return nullptr; }},
                      EnvFactory{"FaultInjection", []() -> Env* { return new FaultInjectionEnv; }}),
    [](const ::testing::TestParamInfo<EnvFactory>& info) { return info.param.name; });

}  // namespace
}  // namespace lsmkv
