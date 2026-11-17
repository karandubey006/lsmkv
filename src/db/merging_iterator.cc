#include "db/merging_iterator.h"

#include <algorithm>

#include "db/dbformat.h"

namespace lsmkv {
namespace {

// The valid children live in a binary heap ordered by current key, smallest
// on top, so Next() costs O(log n) in the number of children.
class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) child->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    // pop_heap moves the smallest child to the back, outside the heap, so
    // advancing it can't break the heap order.
    std::pop_heap(heap_.begin(), heap_.end(), HasLargerKey);
    Iterator* advanced = heap_.back();
    advanced->Next();
    if (advanced->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), HasLargerKey);
    } else {
      heap_.pop_back();
    }
  }

  std::string_view key() const override { return heap_.front()->key(); }
  std::string_view value() const override { return heap_.front()->value(); }

  Status status() const override {
    for (const auto& child : children_) {
      Status s = child->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  // The std heap functions keep the "largest" element on top; inverting the
  // comparison puts the smallest key there.
  static bool HasLargerKey(const Iterator* a, const Iterator* b) {
    return CompareInternalKeys(a->key(), b->key()) > 0;
  }

  void RebuildHeap() {
    heap_.clear();
    for (auto& child : children_) {
      if (child->Valid()) heap_.push_back(child.get());
    }
    std::make_heap(heap_.begin(), heap_.end(), HasLargerKey);
  }

  const std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;  // the valid children; points into children_
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsmkv
