#include "table/merger.h"
#include <cassert>

namespace leveldb {

MergingIterator::MergingIterator(const Comparator* comparator, Iterator** children, int n)
    : comparator_(comparator),
      children_(children, children + n),
      current_(nullptr),
      direction_(kForward) {}

MergingIterator::~MergingIterator() {
    for (size_t i = 0; i < children_.size(); i++) {
        delete children_[i];
    }
}

bool MergingIterator::Valid() const {
    return (current_ != nullptr);
}

Status MergingIterator::status() const {
    Status status;
    for (size_t i = 0; i < children_.size(); i++) {
        const Status s = children_[i]->status();
        if (!s.ok()) {
            status = s;
            break;
        }
    }
    return status;
}

Slice MergingIterator::key() const {
    assert(Valid());
    return current_->key();
}

Slice MergingIterator::value() const {
    assert(Valid());
    return current_->value();
}

void MergingIterator::FindSmallest() {
    Iterator* smallest = nullptr;
    for (size_t i = 0; i < children_.size(); i++) {
        Iterator* child = children_[i];
        if (child->Valid()) {
            if (smallest == nullptr) {
                smallest = child;
            } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
                smallest = child;
            }
        }
    }
    current_ = smallest;
}

void MergingIterator::FindLargest() {
    Iterator* largest = nullptr;
    for (int i = static_cast<int>(children_.size()) - 1; i >= 0; i--) {
        Iterator* child = children_[i];
        if (child->Valid()) {
            if (largest == nullptr) {
                largest = child;
            } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
                largest = child;
            }
        }
    }
    current_ = largest;
}

void MergingIterator::SeekToFirst() {
    for (size_t i = 0; i < children_.size(); i++) {
        children_[i]->SeekToFirst();
    }
    FindSmallest();
    direction_ = kForward;
}

void MergingIterator::SeekToLast() {
    for (size_t i = 0; i < children_.size(); i++) {
        children_[i]->SeekToLast();
    }
    FindLargest();
    direction_ = kReverse;
}

void MergingIterator::Seek(const Slice& target) {
    for (size_t i = 0; i < children_.size(); i++) {
        children_[i]->Seek(target);
    }
    FindSmallest();
    direction_ = kForward;
}

void MergingIterator::Next() {
    assert(Valid());
    if (direction_ != kForward) {
        for (size_t i = 0; i < children_.size(); i++) {
            Iterator* child = children_[i];
            if (child != current_) {
                child->Seek(key());
                if (child->Valid() && comparator_->Compare(key(), child->key()) == 0) {
                    child->Next();
                }
            }
        }
        direction_ = kForward;
    }
    current_->Next();
    FindSmallest();
}

void MergingIterator::Prev() {
    assert(Valid());
    if (direction_ != kReverse) {
        for (size_t i = 0; i < children_.size(); i++) {
            Iterator* child = children_[i];
            if (child != current_) {
                child->Seek(key());
                if (child->Valid()) {
                    child->Prev();
                } else {
                    child->SeekToLast();
                }
            }
        }
        direction_ = kReverse;
    }
    current_->Prev();
    FindLargest();
}

Iterator* NewMergingIterator(const Comparator* cmp, Iterator** list, int n) {
    assert(n >= 0);
    if (n == 0) {
        return NewEmptyIterator();
    } else if (n == 1) {
        return list[0];
    } else {
        return new MergingIterator(cmp, list, n);
    }
}

}  // namespace leveldb
