#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WriteBatch {
public:
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void Put(const Slice& key, const Slice& value) = 0;
        virtual void Delete(const Slice& key) = 0;
    };

    WriteBatch();
    ~WriteBatch() = default;

    // Store the mapping "key->value" in the database.
    void Put(const Slice& key, const Slice& value);

    // If the database contains a mapping for "key", erase it.
    void Delete(const Slice& key);

    // Clear all updates
    void Clear();

    // The size of the database changes caused by this batch.
    size_t ApproximateSize() const { return rep_.size(); }

    // Support for iterating over the contents of a batch.
    Status Iterate(Handler* handler) const;

    // Support for setting/getting sequence number
    static int Count(const WriteBatch* batch);
    static void SetCount(WriteBatch* batch, int n);
    static uint64_t Sequence(const WriteBatch* batch);
    static void SetSequence(WriteBatch* batch, uint64_t seq);

    Slice Data() const { return Slice(rep_); }
    void Append(const WriteBatch& src);

private:
    friend class WriteBatchInternal;
    std::string rep_;  // 8-byte sequence + 4-byte count + entries
};

}  // namespace leveldb
