#pragma once

#include "leveldb/iterator.h"
#include "leveldb/options.h"

namespace leveldb {

typedef Iterator* (*BlockFunction)(void* arg, const ReadOptions& options, const Slice& index_value);

// TwoLevelIterator yields the concatenation of all blocks.
// "index_iter" yields (key, value) pairs where "value" is an encoded
// block handle passed to (*block_function) to open a data block iterator.
Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    BlockFunction block_function,
    void* arg,
    const ReadOptions& options);

}  // namespace leveldb
