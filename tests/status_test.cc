#include "lsmkv/status.h"

#include <gtest/gtest.h>

#include <utility>

namespace lsmkv {
namespace {

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, ErrorCodesAndPredicates) {
  EXPECT_TRUE(Status::NotFound("x").IsNotFound());
  EXPECT_TRUE(Status::Corruption("x").IsCorruption());
  EXPECT_TRUE(Status::NotSupported("x").IsNotSupported());
  EXPECT_TRUE(Status::InvalidArgument("x").IsInvalidArgument());
  EXPECT_TRUE(Status::IOError("x").IsIOError());
  EXPECT_FALSE(Status::NotFound("x").ok());
  EXPECT_FALSE(Status::NotFound("x").IsCorruption());
}

TEST(StatusTest, ToStringJoinsMessages) {
  EXPECT_EQ(Status::Corruption("wal", "bad crc").ToString(), "Corruption: wal: bad crc");
  EXPECT_EQ(Status::IOError("open").ToString(), "IOError: open");
}

TEST(StatusTest, CopyIsDeep) {
  Status a = Status::IOError("disk", "full");
  Status b = a;
  EXPECT_EQ(b.ToString(), a.ToString());
  a = Status::OK();
  EXPECT_TRUE(a.ok());
  EXPECT_EQ(b.ToString(), "IOError: disk: full");
}

TEST(StatusTest, MoveLeavesSourceOk) {
  Status a = Status::NotFound("k");
  Status b = std::move(a);
  EXPECT_TRUE(b.IsNotFound());
  EXPECT_TRUE(a.ok());  // NOLINT(bugprone-use-after-move): moved-from unique_ptr is null
}

}  // namespace
}  // namespace lsmkv
