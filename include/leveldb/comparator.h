#pragma once

#include "leveldb/slice.h"

namespace leveldb {

class Comparator {
public:
    virtual ~Comparator() = default;

    // Three-way comparison:
    // < 0 iff "a" < "b"
    // == 0 iff "a" == "b"
    // > 0 iff "a" > "b"
    virtual int Compare(const Slice& a, const Slice& b) const = 0;

    // The name of the comparator to detect mismatches across DB open
    virtual const char* Name() const = 0;

    // Shortest separator: modifies *start to a shorter string >= *start and < limit
    virtual void FindShortestSeparator(std::string* start, const Slice& limit) const = 0;

    // Shortest successor: modifies *key to a shorter string >= *key
    virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Return a builtin bytewise comparator
const Comparator* BytewiseComparator();

}  // namespace leveldb
