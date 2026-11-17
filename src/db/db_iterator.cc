#include "db/db_iterator.h"

#include <string>

namespace lsmkv {
namespace {

class DBIterator final : public Iterator {
 public:
  DBIterator(std::shared_ptr<const void> pin, std::unique_ptr<Iterator> internal,
             SequenceNumber snapshot)
      : pin_(std::move(pin)), internal_(std::move(internal)), snapshot_(snapshot) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    internal_->SeekToFirst();
    FindVisibleEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    internal_->Seek(SeekKey(target, snapshot_));
    FindVisibleEntry(/*skipping=*/false);
  }

  void Next() override {
    // Every older version of the current key comes next; pass over them.
    skip_.assign(key());
    internal_->Next();
    FindVisibleEntry(/*skipping=*/true);
  }

  std::string_view key() const override { return UserKeyOf(internal_->key()); }
  std::string_view value() const override { return internal_->value(); }

  Status status() const override {
    if (!status_.ok()) return status_;
    return internal_->status();
  }

 private:
  // Advances internal_ to the newest visible version of the next user key that
  // isn't deleted. With `skipping`, entries for skip_ are passed over.
  void FindVisibleEntry(bool skipping) {
    valid_ = false;
    for (; internal_->Valid(); internal_->Next()) {
      const std::optional<InternalKeyParts> parts = DecodeInternalKey(internal_->key());
      if (!parts) {
        status_ = Status::Corruption("malformed internal key");
        return;
      }
      if (parts->sequence > snapshot_) continue;  // written after the snapshot
      if (skipping && parts->user_key == skip_) continue;
      if (parts->type == ValueType::kDeletion) {
        skip_.assign(parts->user_key);  // hides every older version too
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
  }

  // Declared before internal_, so it is destroyed after it.
  const std::shared_ptr<const void> pin_;
  const std::unique_ptr<Iterator> internal_;
  const SequenceNumber snapshot_;
  bool valid_ = false;
  std::string skip_;  // user key whose remaining versions are hidden
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::shared_ptr<const void> pin,
                                        std::unique_ptr<Iterator> internal,
                                        SequenceNumber snapshot) {
  return std::make_unique<DBIterator>(std::move(pin), std::move(internal), snapshot);
}

}  // namespace lsmkv
