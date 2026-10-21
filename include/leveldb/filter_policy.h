#pragma once

#include <cstddef>
#include <string>
#include "leveldb/slice.h"

namespace leveldb {

class FilterPolicy {
public:
    virtual ~FilterPolicy() = default;
    virtual const char* Name() const = 0;
    virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;
    virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Return a new bloom filter policy with specified bits per key (typically 10)
const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}  // namespace leveldb
