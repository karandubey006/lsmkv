#include "wal/wal.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "env/fault_injection_env.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

using Outcome = WalReader::Outcome;

// Every record the reader returns before it stops, plus how it stopped.
struct ReadBack {
  std::vector<std::string> records;
  Outcome stop;
  uint64_t good_end;
};

ReadBack ReadAll(std::string contents) {
  WalReader reader(std::move(contents));
  ReadBack out;
  std::string_view payload;
  while ((out.stop = reader.Next(&payload)) == Outcome::kRecord) out.records.emplace_back(payload);
  out.good_end = reader.good_end();
  // The outcome sticks.
  EXPECT_EQ(reader.Next(&payload), out.stop);
  return out;
}

// Records of assorted sizes, including ones that straddle 4 KB pages.
std::vector<std::string> SampleRecords() {
  // A payload of one zero byte is valid: only the length 0 is reserved.
  std::vector<std::string> records = {"a", "hello", std::string(4096, 'x'), std::string(1, '\0'),
                                      std::string(10000, 'y')};
  std::mt19937 rng(7);
  for (int i = 0; i < 20; ++i) {
    std::string r(rng() % 300 + 1, '\0');
    for (char& c : r) c = static_cast<char>(rng());
    records.push_back(std::move(r));
  }
  return records;
}

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_OK(env_.CreateDirectory("/db"));
    ASSERT_OK(env_.OpenDirectory("/db", &dir_));
  }

  std::unique_ptr<WalWriter> NewWriter(std::string_view name) {
    std::unique_ptr<WritableFile> file;
    EXPECT_OK(dir_->Create(name, &file));
    return std::make_unique<WalWriter>(std::move(file));
  }

  // Writes records to a WAL and returns the file's bytes.
  std::string Encode(const std::vector<std::string>& records) {
    auto w = NewWriter("encode.log");
    for (const auto& r : records) EXPECT_OK(w->AddRecord({r}));
    EXPECT_OK(w->Close());
    std::string contents;
    EXPECT_OK(ReadEntireFile(*dir_, "encode.log", &contents));
    EXPECT_OK(dir_->Remove("encode.log"));
    return contents;
  }

  FaultInjectionEnv env_;
  std::unique_ptr<Directory> dir_;
};

TEST_F(WalTest, RoundTrip) {
  const auto records = SampleRecords();
  const std::string contents = Encode(records);
  ReadBack got = ReadAll(contents);
  EXPECT_EQ(got.records, records);
  EXPECT_EQ(got.stop, Outcome::kEnd);
  EXPECT_EQ(got.good_end, contents.size());
}

TEST_F(WalTest, EmptyFileIsACleanEnd) {
  ReadBack got = ReadAll("");
  EXPECT_TRUE(got.records.empty());
  EXPECT_EQ(got.stop, Outcome::kEnd);
}

TEST_F(WalTest, HeaderLayout) {
  const std::string contents = Encode({"abc"});
  ASSERT_EQ(contents.size(), kWalHeaderSize + 3);
  EXPECT_EQ(DecodeFixed32(contents.data() + 4), 3u);
  EXPECT_EQ(contents.substr(kWalHeaderSize), "abc");
}

TEST_F(WalTest, MultiPartRecordEqualsConcatenation) {
  auto w = NewWriter("log");
  ASSERT_OK(w->AddRecord({"seq-", "", "batch"}));
  ASSERT_OK(w->Close());
  std::string contents;
  ASSERT_OK(ReadEntireFile(*dir_, "log", &contents));
  EXPECT_EQ(contents, Encode({"seq-batch"}));
}

TEST_F(WalTest, EmptyPayloadIsRejectedAndNotSticky) {
  auto w = NewWriter("log");
  EXPECT_TRUE(w->AddRecord({}).IsInvalidArgument());
  EXPECT_TRUE(w->AddRecord({"", ""}).IsInvalidArgument());
  EXPECT_OK(w->AddRecord({"fine"}));
}

// Cutting the file at every byte: the reader returns exactly the records that
// are wholly inside the cut, and reports a clean end only at a boundary.
TEST_F(WalTest, EveryTruncationPointYieldsACompletePrefix) {
  const std::vector<std::string> records = {"first", std::string(300, 'm'), "third"};
  const std::string contents = Encode(records);
  std::vector<size_t> boundaries = {0};
  for (const auto& r : records) boundaries.push_back(boundaries.back() + kWalHeaderSize + r.size());

  for (size_t cut = 0; cut <= contents.size(); ++cut) {
    ReadBack got = ReadAll(contents.substr(0, cut));
    size_t complete = 0;
    while (complete < records.size() && boundaries[complete + 1] <= cut) ++complete;
    ASSERT_EQ(got.records.size(), complete) << "cut " << cut;
    for (size_t i = 0; i < complete; ++i) ASSERT_EQ(got.records[i], records[i]);
    ASSERT_EQ(got.good_end, boundaries[complete]) << "cut " << cut;
    ASSERT_EQ(got.stop, cut == boundaries[complete] ? Outcome::kEnd : Outcome::kBadRecord)
        << "cut " << cut;
  }
}

// Flipping any single bit: records before the damaged one are returned
// intact, the damaged one is reported bad, and nothing wrong is ever returned.
TEST_F(WalTest, EveryBitFlipIsDetected) {
  const std::vector<std::string> records = {"one", "two-two", "three"};
  const std::string contents = Encode(records);
  size_t record_start = 0, record_index = 0;
  for (size_t bit = 0; bit < contents.size() * 8; ++bit) {
    const size_t byte = bit / 8;
    while (byte >= record_start + kWalHeaderSize + records[record_index].size()) {
      record_start += kWalHeaderSize + records[record_index].size();
      ++record_index;
    }
    std::string damaged = contents;
    damaged[byte] ^= static_cast<char>(1 << (bit % 8));
    ReadBack got = ReadAll(damaged);
    ASSERT_EQ(got.stop, Outcome::kBadRecord) << "bit " << bit;
    ASSERT_EQ(got.good_end, record_start) << "bit " << bit;
    ASSERT_EQ(got.records, std::vector<std::string>(records.begin(), records.begin() + record_index))
        << "bit " << bit;
  }
}

TEST_F(WalTest, ZeroFilledTailIsBad) {
  const std::string contents = Encode({"good"});
  for (size_t zeros : {1, 7, 8, 100, 4096}) {
    ReadBack got = ReadAll(contents + std::string(zeros, '\0'));
    EXPECT_EQ(got.records, std::vector<std::string>{"good"});
    EXPECT_EQ(got.stop, Outcome::kBadRecord);
    EXPECT_EQ(got.good_end, contents.size());
  }
}

TEST_F(WalTest, HugeGarbageLengthDoesNotReadPastTheEnd) {
  std::string contents(kWalHeaderSize + 10, 'z');
  EncodeFixed32(contents.data() + 4, 0xFFFFFFF0u);
  ReadBack got = ReadAll(contents);
  EXPECT_EQ(got.stop, Outcome::kBadRecord);
  EXPECT_EQ(got.good_end, 0u);
}

// Fails the Nth Append (counting from 1); everything else succeeds.
class FailingFile final : public WritableFile {
 public:
  explicit FailingFile(int fail_on) : fail_on_(fail_on) {}
  Status Append(std::string_view) override {
    return ++appends_ == fail_on_ ? Status::IOError("injected") : Status::OK();
  }
  Status Sync() override {
    ++syncs_;
    return Status::OK();
  }
  Status Close() override { return Status::OK(); }
  int appends_ = 0, syncs_ = 0;

 private:
  const int fail_on_;
};

TEST(WalWriterErrors, IoErrorsAreSticky) {
  auto owned = std::make_unique<FailingFile>(2);
  FailingFile* file = owned.get();
  WalWriter w(std::move(owned));
  ASSERT_OK(w.AddRecord({"ok"}));
  EXPECT_TRUE(w.AddRecord({"fails"}).IsIOError());
  // The file would accept these, but the writer must refuse them.
  EXPECT_TRUE(w.AddRecord({"after"}).IsIOError());
  EXPECT_TRUE(w.Sync().IsIOError());
  EXPECT_EQ(file->appends_, 2);
  EXPECT_EQ(file->syncs_, 0);
}

// Simulated power loss: every synced record survives, and what's recovered is
// always a prefix of what was written.
TEST(WalPowerLoss, RecoversAPrefixContainingEverySyncedRecord) {
  std::mt19937 rng(99);
  for (uint64_t seed = 0; seed < 300; ++seed) {
    FaultInjectionEnv env;
    ASSERT_OK(env.CreateDirectory("/db"));
    std::unique_ptr<Directory> dir;
    ASSERT_OK(env.OpenDirectory("/db", &dir));
    std::unique_ptr<WritableFile> file;
    ASSERT_OK(dir->Create("000001.log", &file));
    ASSERT_OK(dir->Sync());
    WalWriter w(std::move(file));

    std::vector<std::string> written;
    size_t synced = 0;
    const int n = static_cast<int>(rng() % 40) + 1;
    for (int i = 0; i < n; ++i) {
      std::string r = "record-" + std::to_string(i) + "-" + std::string(rng() % 2000, 'p');
      ASSERT_OK(w.AddRecord({r}));
      written.push_back(std::move(r));
      if (rng() % 4 == 0) {
        ASSERT_OK(w.Sync());
        synced = written.size();
      }
    }
    env.SimulatePowerLoss(FaultInjectionEnv::PowerLossMode::kTornWrites, seed);

    ASSERT_OK(env.OpenDirectory("/db", &dir));
    std::string contents;
    ASSERT_OK(ReadEntireFile(*dir, "000001.log", &contents));
    ReadBack got = ReadAll(std::move(contents));
    ASSERT_GE(got.records.size(), synced) << "seed " << seed;
    ASSERT_LE(got.records.size(), written.size()) << "seed " << seed;
    for (size_t i = 0; i < got.records.size(); ++i) {
      ASSERT_EQ(got.records[i], written[i]) << "seed " << seed << " record " << i;
    }
  }
}

}  // namespace
}  // namespace lsmkv
