#pragma once

// A sorted set with a single writer and lock-free readers.
//
// Thread safety: Insert() requires external synchronization. Contains() and
// Iterator may run concurrently with Insert() without locking.
//
// Nodes are never removed. Insert() fully initializes a node before linking
// it with a release store, and readers follow links with acquire loads.
//
// Nodes are allocated from the Arena passed to the constructor, which must
// outlive the SkipList and every Iterator over it.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <new>
#include <random>

#include "memtable/arena.h"

namespace lsmkv {

// Compare: int operator()(const Key&, const Key&) returning <0, 0 or >0.
// Key must be trivially copyable.
template <typename Key, typename Compare>
class SkipList {
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;

  SkipList(Compare compare, Arena* arena)
      : compare_(compare), arena_(arena), head_(MakeNode(Key{}, kMaxHeight)) {}
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: external synchronization.
  // REQUIRES: no key comparing equal to `key` is present.
  void Insert(const Key& key);

  bool Contains(const Key& key) const {
    const Node* n = LowerBound(key, nullptr);
    return n != nullptr && compare_(n->key, key) == 0;
  }

  // Sees every key inserted before it was positioned; may or may not see keys
  // inserted concurrently.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    void SeekToFirst() { node_ = list_->head_->AcquireNext(0); }
    // Positions at the first key >= target.
    void Seek(const Key& target) { node_ = list_->LowerBound(target, nullptr); }

    // REQUIRES: Valid().
    void Next() {
      assert(Valid());
      node_ = node_->AcquireNext(0);
    }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  // Allocated as one block: [Node][std::atomic<Node*> x height].
  struct Node {
    Node(const Key& k, std::atomic<Node*>* links) : key(k), links_(links) {}

    Node* AcquireNext(int level) const { return links_[level].load(std::memory_order_acquire); }
    void ReleaseSetNext(int level, Node* n) { links_[level].store(n, std::memory_order_release); }
    Node* RelaxedNext(int level) const { return links_[level].load(std::memory_order_relaxed); }
    void RelaxedSetNext(int level, Node* n) { links_[level].store(n, std::memory_order_relaxed); }

    const Key key;

   private:
    std::atomic<Node*>* const links_;
  };
  static_assert(std::atomic<Node*>::is_always_lock_free);

  Node* MakeNode(const Key& key, int height);
  int PickHeight();

  // Returns the first node with key >= `key`, or nullptr. If preds is not
  // null, sets preds[i] to the last node before `key` on every level i.
  Node* LowerBound(const Key& key, Node** preds) const;

  const Compare compare_;
  Arena* const arena_;
  Node* const head_;  // sentinel with kMaxHeight levels; its key is unused
  std::mt19937 rng_;  // writer only
};

template <typename Key, typename Compare>
typename SkipList<Key, Compare>::Node* SkipList<Key, Compare>::MakeNode(const Key& key,
                                                                        int height) {
  using Link = std::atomic<Node*>;
  static_assert(sizeof(Node) % alignof(Link) == 0);
  char* mem = arena_->Allocate(sizeof(Node) + sizeof(Link) * size_t(height), alignof(Node));
  Link* links = new (mem + sizeof(Node)) Link[height]();
  return new (mem) Node(key, links);
}

template <typename Key, typename Compare>
int SkipList<Key, Compare>::PickHeight() {
  // Every two trailing zero bits add a level, so each extra level has
  // probability 1/4.
  const int height = 1 + std::countr_zero(static_cast<uint32_t>(rng_())) / 2;
  return std::min(height, kMaxHeight);
}

template <typename Key, typename Compare>
typename SkipList<Key, Compare>::Node* SkipList<Key, Compare>::LowerBound(const Key& key,
                                                                          Node** preds) const {
  Node* pred = head_;
  for (int level = kMaxHeight - 1; level >= 0; --level) {
    Node* next = pred->AcquireNext(level);
    while (next != nullptr && compare_(next->key, key) < 0) {
      pred = next;
      next = pred->AcquireNext(level);
    }
    if (preds != nullptr) preds[level] = pred;
    if (level == 0) return next;
  }
  return nullptr;  // unreachable
}

template <typename Key, typename Compare>
void SkipList<Key, Compare>::Insert(const Key& key) {
  Node* preds[kMaxHeight];
  [[maybe_unused]] Node* existing = LowerBound(key, preds);
  assert(existing == nullptr || compare_(existing->key, key) != 0);

  const int height = PickHeight();
  Node* node = MakeNode(key, height);
  // Link from the bottom up, so a node found on some level is already linked
  // on every level below it.
  for (int level = 0; level < height; ++level) {
    // Relaxed: no reader can reach `node` on this level until the release
    // store below, and only this thread writes links.
    node->RelaxedSetNext(level, preds[level]->RelaxedNext(level));
    preds[level]->ReleaseSetNext(level, node);
  }
}

}  // namespace lsmkv
