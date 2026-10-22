#pragma once

#include <string>
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/write_batch.h"

namespace leveldb {

class DB {
public:
    // Open the database with the specified "name".
    // Stores a pointer to a heap-allocated database in *dbptr and returns
    // OK on success.
    // Stores nullptr in *dbptr and returns an error on failure.
    // The caller should delete *dbptr when it is no longer needed.
    static Status Open(const Options& options, const std::string& name, DB** dbptr);

    DB() = default;
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    virtual ~DB() = default;

    // Set the database entry for "key" to "value". Returns OK on success,
    // and a non-OK status on error.
    // Note: consider setting options.sync = true.
    virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;

    // Remove the database entry (if any) for "key". Returns OK on
    // success, and a non-OK status on error.
    // It is not an error if "key" did not exist in the database.
    virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

    // Apply the specified updates to the database.
    // Returns OK on success, non-OK on failure.
    // Note: consider setting options.sync = true.
    virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

    // If the database contains an entry for "key" store the
    // corresponding value in *value and return OK.
    //
    // If there is no entry for "key" leave *value unchanged and return
    // a status for which Status::IsNotFound() returns true.
    virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;

    // Return a heap-allocated iterator over the contents of the database.
    // The caller should delete the iterator when it is no longer needed.
    // The returned iterator is initially invalid (the caller must
    // call one of the Seek methods on the iterator before using it).
    virtual Iterator* NewIterator(const ReadOptions& options) = 0;

    // Compact the underlying storage for the key range [*begin,*end].
    // If begin==nullptr or end==nullptr, compactions are not bounded.
    virtual void CompactRange(const Slice* begin, const Slice* end) = 0;
};

// Destroy the contents of the specified database.
Status DestroyDB(const std::string& name, const Options& options);

}  // namespace leveldb
