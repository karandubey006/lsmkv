#include "memtable/memtable.h"

#include <cassert>
#include <cstring>

#include "util/coding.h"

namespace lsmkv {
namespace {

// The value stored after an entry's internal key.
std::string_view ValueOf(std::string_view internal_key) {
  const char* p = internal_key.data() + internal_key.size();
  uint32_t size = 0;
  p = GetVarint32Ptr(p, p + 5, &size);  // written by Add(), so well-formed
  assert(p != nullptr);
  return {p, size};
}

}  // namespace

void MemTable::Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
                   std::string_view value) {
  if (type == ValueType::kDeletion) value = {};
  const size_t key_size = user_key.size() + kTrailerSize;
  const size_t size = key_size + static_cast<size_t>(VarintLength(value.size())) + value.size();

  char* const entry = arena_.Allocate(size);
  std::memcpy(entry, user_key.data(), user_key.size());
  EncodeFixed64(entry + user_key.size(), PackTrailer(sequence, type));
  char* p = EncodeVarint32(entry + key_size, static_cast<uint32_t>(value.size()));
  std::memcpy(p, value.data(), value.size());
  assert(p + value.size() == entry + size);

  table_.Insert(std::string_view(entry, key_size));
}

MemTable::GetResult MemTable::Get(std::string_view user_key, SequenceNumber snapshot,
                                  std::string* value) const {
  Table::Iterator it(&table_);
  it.Seek(SeekKey(user_key, snapshot));
  if (!it.Valid() || UserKeyOf(it.key()) != user_key) return GetResult::kAbsent;

  if (TypeOf(it.key()) == ValueType::kDeletion) return GetResult::kDeleted;
  value->assign(ValueOf(it.key()));
  return GetResult::kFound;
}

class MemTable::TableIterator final : public Iterator {
 public:
  explicit TableIterator(const Table* table) : it_(table) {}

  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view target) override { it_.Seek(target); }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return it_.key(); }
  std::string_view value() const override { return ValueOf(it_.key()); }
  Status status() const override { return Status::OK(); }

 private:
  Table::Iterator it_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<TableIterator>(&table_);
}

}  // namespace lsmkv
