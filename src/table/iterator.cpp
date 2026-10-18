#include "leveldb/iterator.h"

namespace leveldb {

namespace {

class EmptyIterator : public Iterator {
public:
    EmptyIterator(const Status& s) : status_(s) {}
    bool Valid() const override { return false; }
    void Seek(const Slice& /*target*/) override {}
    void SeekToFirst() override {}
    void SeekToLast() override {}
    void Next() override { assert(false); }
    void Prev() override { assert(false); }
    Slice key() const override {
        assert(false);
        return Slice();
    }
    Slice value() const override {
        assert(false);
        return Slice();
    }
    Status status() const override { return status_; }

private:
    Status status_;
};

}  // namespace

Iterator* NewEmptyIterator() {
    return new EmptyIterator(Status::OK());
}

Iterator* NewErrorIterator(const Status& status) {
    return new EmptyIterator(status);
}

}  // namespace leveldb
