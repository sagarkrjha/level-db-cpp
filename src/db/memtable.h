#pragma once

#include <string>
#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/iterator.h"
#include "util/arena.h"

namespace leveldb {

class MemTable {
public:
    explicit MemTable(const InternalKeyComparator& comparator);

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // Increase reference count
    void Ref() { ++refs_; }
    // Drop reference count. Delete if no more refs.
    void Unref() {
        --refs_;
        assert(refs_ >= 0);
        if (refs_ <= 0) {
            delete this;
        }
    }

    // Returns an estimate of the number of bytes of data in use.
    size_t ApproximateMemoryUsage();

    // Return an iterator that yields the contents of the memtable.
    Iterator* NewIterator();

    // Add an entry into memtable that maps key to value at the
    // specified sequence number and with the specified type.
    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

    // If memtable contains a value for key, store it in *value and return true.
    // If memtable contains a deletion for key, store NotFound() in *status and return true.
    // Else, return false.
    bool Get(const Slice& user_key, SequenceNumber seq, std::string* value, Status* s);

    friend class MemTableIterator;

private:
    ~MemTable();  // Private destructor to enforce Ref()/Unref()

    struct KeyComparator {
        const InternalKeyComparator comparator;
        explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
        int operator()(const char* a, const char* b) const;
    };

    typedef SkipList<const char*, KeyComparator> Table;

    KeyComparator comparator_;
    int refs_;
    Arena arena_;
    Table table_;
};

}  // namespace leveldb
