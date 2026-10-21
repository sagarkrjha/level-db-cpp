#pragma once

#include <vector>
#include "leveldb/comparator.h"
#include "leveldb/iterator.h"

namespace leveldb {

class MergingIterator : public Iterator {
public:
    MergingIterator(const Comparator* comparator, Iterator** children, int n);
    ~MergingIterator() override;

    bool Valid() const override;
    void SeekToFirst() override;
    void SeekToLast() override;
    void Seek(const Slice& target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;
    Status status() const override;

private:
    void FindSmallest();
    void FindLargest();

    const Comparator* comparator_;
    std::vector<Iterator*> children_;
    Iterator* current_;
    enum Direction { kForward, kReverse };
    Direction direction_;
};

Iterator* NewMergingIterator(const Comparator* cmp, Iterator** list, int n);

}  // namespace leveldb
