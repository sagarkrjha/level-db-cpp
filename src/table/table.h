#pragma once

#include <cstdint>
#include <memory>
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "table/format.h"

namespace leveldb {

class RandomAccessFile;
class Block;

class Table {
public:
    // Attempt to open the table that is stored in bytes [0..size)
    // of "file", and read the metadata entries of the table.
    // If successful, returns ok and sets "*table" to the newly opened
    // table. The client should delete "*table" when no longer needed.
    static Status Open(const Options& options,
                       RandomAccessFile* file,
                       uint64_t size,
                       Table** table);

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    ~Table();

    // Given a key, return an iterator that yields the corresponding value.
    Iterator* NewIterator(const ReadOptions& options) const;

    // Approximate internal key search using internal filter
    bool InternalGet(const ReadOptions& options, const Slice& key,
                     void* arg,
                     void (*handle_result)(void* arg, const Slice& k, const Slice& v));

private:
    struct Rep;
    explicit Table(Rep* rep) : rep_(rep) {}

    static Iterator* BlockReader(void* arg, const ReadOptions& options, const Slice& index_value);

    Rep* rep_;
};

}  // namespace leveldb
