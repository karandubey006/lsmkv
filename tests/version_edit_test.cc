#include "db/version_edit.h"

#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "db/version.h"
#include "test_util.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq = 1) {
  std::string key;
  AppendInternalKey(&key, user_key, seq, ValueType::kValue);
  return key;
}

SstInfo Sst(uint64_t number, std::string_view smallest, std::string_view largest,
            uint64_t file_size = 4096) {
  return SstInfo{number, file_size, IKey(smallest), IKey(largest)};
}

VersionEdit FullEdit() {
  VersionEdit edit;
  edit.log_number = 12;
  edit.next_file_number = 300;
  edit.last_sequence = kMaxSequenceNumber;
  edit.RemoveFile(0, 5);
  edit.RemoveFile(kNumLevels - 1, uint64_t{1} << 40);
  edit.AddFile(0, Sst(17, "a", "z", 0));
  edit.AddFile(3, SstInfo{UINT64_MAX, UINT64_MAX, IKey(std::string("\0\xff", 2)), IKey("\xff")});
  edit.AddFile(kNumLevels - 1, Sst(18, "", "", 1));
  return edit;
}

VersionEdit Decode(std::string_view encoded) {
  VersionEdit edit;
  EXPECT_OK(edit.DecodeFrom(encoded));
  return edit;
}

TEST(VersionEditTest, RoundTripEveryField) {
  const VersionEdit edit = FullEdit();
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_EQ(Decode(encoded), edit);
}

TEST(VersionEditTest, EmptyEdit) {
  std::string encoded;
  VersionEdit().EncodeTo(&encoded);
  EXPECT_TRUE(encoded.empty());
  EXPECT_EQ(Decode(encoded), VersionEdit());
}

TEST(VersionEditTest, DecodeReplacesPreviousContents) {
  VersionEdit edit = FullEdit();
  VersionEdit small;
  small.last_sequence = 3;
  std::string encoded;
  small.EncodeTo(&encoded);
  ASSERT_OK(edit.DecodeFrom(encoded));
  EXPECT_EQ(edit, small);
}

TEST(VersionEditTest, ManyFiles) {
  std::mt19937_64 rng(42);
  VersionEdit edit;
  edit.next_file_number = 100000;
  for (uint64_t i = 0; i < 5000; ++i) {
    std::string a(rng() % 40, '\0'), b(rng() % 40, '\0');
    for (char& c : a) c = static_cast<char>(rng());
    for (char& c : b) c = static_cast<char>(rng());
    edit.AddFile(static_cast<int>(rng() % kNumLevels),
                 SstInfo{i, rng(), IKey(a, rng() % 1000), IKey(b, rng() % 1000)});
    if (i % 3 == 0) edit.RemoveFile(static_cast<int>(rng() % kNumLevels), rng());
  }
  std::string encoded;
  edit.EncodeTo(&encoded);
  EXPECT_EQ(Decode(encoded), edit);
}

TEST(VersionEditTest, UnknownTagIsCorruption) {
  for (uint32_t tag : {0u, 6u, 7u, 127u, 128u, 1u << 20}) {
    std::string encoded;
    PutVarint32(&encoded, tag);
    PutVarint64(&encoded, 1);
    VersionEdit edit;
    EXPECT_TRUE(edit.DecodeFrom(encoded).IsCorruption()) << tag;
  }
}

TEST(VersionEditTest, TruncationAtEveryByte) {
  // Field boundaries: the encodings of the edit built up field by field, in
  // the order EncodeTo writes them.
  const VersionEdit full = FullEdit();
  std::set<size_t> boundaries;
  VersionEdit partial;
  auto mark = [&] {
    std::string e;
    partial.EncodeTo(&e);
    boundaries.insert(e.size());
  };
  mark();
  partial.log_number = full.log_number;
  mark();
  partial.next_file_number = full.next_file_number;
  mark();
  partial.last_sequence = full.last_sequence;
  mark();
  for (const auto& r : full.removed) {
    partial.removed.push_back(r);
    mark();
  }
  for (const auto& a : full.added) {
    partial.added.push_back(a);
    mark();
  }

  std::string encoded;
  full.EncodeTo(&encoded);
  ASSERT_EQ(*boundaries.rbegin(), encoded.size());
  for (size_t cut = 0; cut < encoded.size(); ++cut) {
    VersionEdit edit;
    const Status s = edit.DecodeFrom(std::string_view(encoded).substr(0, cut));
    if (boundaries.contains(cut)) {
      EXPECT_TRUE(s.ok()) << "cut at " << cut << ": " << s.ToString();
    } else {
      EXPECT_TRUE(s.IsCorruption()) << "cut at " << cut << ": " << s.ToString();
    }
  }
}

TEST(VersionEditTest, TrailingGarbageIsCorruption) {
  std::string encoded;
  FullEdit().EncodeTo(&encoded);
  for (std::string garbage : {std::string(1, '\0'), std::string("\x01"), std::string("\xff"),
                              std::string("\x02\x00", 2), std::string("\x05")}) {
    VersionEdit edit;
    EXPECT_TRUE(edit.DecodeFrom(encoded + garbage).IsCorruption()) << test::Printable(garbage);
  }
}

TEST(VersionEditTest, LevelOutOfRangeIsCorruption) {
  VersionEdit edit;
  std::string added;
  PutVarint32(&added, 1);  // added file
  PutVarint32(&added, kNumLevels);
  PutVarint64(&added, 7);
  PutVarint64(&added, 100);
  PutLengthPrefixed(&added, IKey("a"));
  PutLengthPrefixed(&added, IKey("b"));
  EXPECT_TRUE(edit.DecodeFrom(added).IsCorruption());

  std::string removed;
  PutVarint32(&removed, 2);  // removed file
  PutVarint32(&removed, 1000);
  PutVarint64(&removed, 7);
  EXPECT_TRUE(edit.DecodeFrom(removed).IsCorruption());
}

TEST(VersionEditTest, RepeatedCounterIsCorruption) {
  for (uint32_t tag : {3u, 4u, 5u}) {
    std::string encoded;
    for (int i = 0; i < 2; ++i) {
      PutVarint32(&encoded, tag);
      PutVarint64(&encoded, 9);
    }
    VersionEdit edit;
    EXPECT_TRUE(edit.DecodeFrom(encoded).IsCorruption()) << tag;
  }
}

TEST(VersionEditTest, RandomBytesNeverCrash) {
  std::mt19937_64 rng(7);
  std::string encoded;
  FullEdit().EncodeTo(&encoded);
  for (int i = 0; i < 20000; ++i) {
    std::string mutated = encoded;
    const int flips = 1 + static_cast<int>(rng() % 4);
    for (int f = 0; f < flips; ++f) mutated[rng() % mutated.size()] = static_cast<char>(rng());
    VersionEdit edit;
    const Status s = edit.DecodeFrom(mutated);
    EXPECT_TRUE(s.ok() || s.IsCorruption());
  }
}

// ---------------------------------------------------------------------------
// Version

std::shared_ptr<const Version> ApplyOk(const Version& base, const VersionEdit& edit) {
  std::shared_ptr<const Version> next;
  EXPECT_OK(base.Apply(edit, &next));
  return next ? next : std::make_shared<const Version>();
}

std::vector<uint64_t> Numbers(const Version& v, int level) {
  std::vector<uint64_t> out;
  for (const auto& f : v.files(level)) out.push_back(f->number);
  return out;
}

void ExpectCorruption(const Version& base, const VersionEdit& edit) {
  std::shared_ptr<const Version> next;
  EXPECT_TRUE(base.Apply(edit, &next).IsCorruption());
  EXPECT_EQ(next, nullptr);
}

TEST(VersionTest, LevelZeroIsNewestFirst) {
  VersionEdit edit;
  edit.AddFile(0, Sst(4, "a", "m"));
  edit.AddFile(0, Sst(9, "c", "z"));
  edit.AddFile(0, Sst(2, "a", "z"));
  auto v = ApplyOk(Version(), edit);
  EXPECT_EQ(Numbers(*v, 0), (std::vector<uint64_t>{9, 4, 2}));

  VersionEdit more;
  more.AddFile(0, Sst(11, "b", "b"));
  more.RemoveFile(0, 4);
  v = ApplyOk(*v, more);
  EXPECT_EQ(Numbers(*v, 0), (std::vector<uint64_t>{11, 9, 2}));
  EXPECT_EQ(v->TotalFiles(), 3u);
}

TEST(VersionTest, HigherLevelsSortedBySmallestKey) {
  VersionEdit edit;
  edit.AddFile(2, Sst(1, "m", "p"));
  edit.AddFile(2, Sst(2, "a", "c"));
  edit.AddFile(2, Sst(3, "q", "z"));
  auto v = ApplyOk(Version(), edit);
  EXPECT_EQ(Numbers(*v, 2), (std::vector<uint64_t>{2, 1, 3}));

  VersionEdit more;
  more.AddFile(2, Sst(4, "d", "f"));
  v = ApplyOk(*v, more);
  EXPECT_EQ(Numbers(*v, 2), (std::vector<uint64_t>{2, 4, 1, 3}));
}

TEST(VersionTest, BaseIsUnchanged) {
  VersionEdit edit;
  edit.AddFile(1, Sst(1, "a", "b"));
  auto v1 = ApplyOk(Version(), edit);
  VersionEdit remove;
  remove.RemoveFile(1, 1);
  auto v2 = ApplyOk(*v1, remove);
  EXPECT_EQ(v1->TotalFiles(), 1u);
  EXPECT_EQ(v2->TotalFiles(), 0u);
}

TEST(VersionTest, DuplicateNumberIsCorruption) {
  VersionEdit edit;
  edit.AddFile(0, Sst(5, "a", "b"));
  auto v = ApplyOk(Version(), edit);
  ExpectCorruption(*v, edit);  // same level

  VersionEdit other_level;
  other_level.AddFile(3, Sst(5, "x", "y"));
  ExpectCorruption(*v, other_level);

  VersionEdit twice;
  twice.AddFile(0, Sst(6, "a", "b"));
  twice.AddFile(1, Sst(6, "a", "b"));
  ExpectCorruption(Version(), twice);
}

TEST(VersionTest, RemovingAbsentFileIsCorruption) {
  VersionEdit add;
  add.AddFile(1, Sst(5, "a", "b"));
  auto v = ApplyOk(Version(), add);

  VersionEdit absent;
  absent.RemoveFile(1, 6);
  ExpectCorruption(*v, absent);

  VersionEdit wrong_level;
  wrong_level.RemoveFile(2, 5);
  ExpectCorruption(*v, wrong_level);

  VersionEdit twice;
  twice.RemoveFile(1, 5);
  twice.RemoveFile(1, 5);
  ExpectCorruption(*v, twice);
}

TEST(VersionTest, MoveFileBetweenLevels) {
  VersionEdit add;
  add.AddFile(1, Sst(5, "a", "b"));
  auto v = ApplyOk(Version(), add);
  VersionEdit move;
  move.RemoveFile(1, 5);
  move.AddFile(2, Sst(5, "a", "b"));
  v = ApplyOk(*v, move);
  EXPECT_TRUE(v->files(1).empty());
  EXPECT_EQ(Numbers(*v, 2), (std::vector<uint64_t>{5}));
}

TEST(VersionTest, OverlapInLevelOneOrMoreIsCorruption) {
  VersionEdit base;
  base.AddFile(1, Sst(1, "c", "f"));
  auto v = ApplyOk(Version(), base);

  for (auto [lo, hi] : std::vector<std::pair<const char*, const char*>>{
           {"a", "c"}, {"f", "g"}, {"d", "e"}, {"a", "z"}, {"e", "h"}}) {
    VersionEdit edit;
    edit.AddFile(1, Sst(2, lo, hi));
    ExpectCorruption(*v, edit);
  }

  // Overlap within one edit.
  VersionEdit pair;
  pair.AddFile(4, Sst(2, "a", "m"));
  pair.AddFile(4, Sst(3, "k", "z"));
  ExpectCorruption(Version(), pair);

  // Same user key, disjoint internal keys: the newer versions come first.
  VersionEdit split;
  split.AddFile(1, SstInfo{2, 1, IKey("g", 9), IKey("k", 9)});
  split.AddFile(1, SstInfo{3, 1, IKey("k", 8), IKey("m", 1)});
  ApplyOk(*v, split);

  // L0 may overlap.
  VersionEdit l0;
  l0.AddFile(0, Sst(4, "a", "z"));
  l0.AddFile(0, Sst(5, "a", "z"));
  ApplyOk(*v, l0);
}

TEST(VersionTest, BadKeyRangeIsCorruption) {
  VersionEdit reversed;
  reversed.AddFile(0, Sst(1, "z", "a"));
  ExpectCorruption(Version(), reversed);

  VersionEdit short_key;
  short_key.AddFile(0, SstInfo{1, 1, "abc", IKey("z")});
  ExpectCorruption(Version(), short_key);

  VersionEdit level;
  level.AddFile(kNumLevels, Sst(1, "a", "b"));
  ExpectCorruption(Version(), level);
}

}  // namespace
}  // namespace lsmkv
