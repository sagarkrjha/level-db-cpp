#pragma once

#include "leveldb/write_batch.h"

namespace leveldb {

class WriteBatchInternal {
public:
    static int Count(const WriteBatch* batch) {
        return WriteBatch::Count(batch);
    }

    static void SetCount(WriteBatch* batch, int n) {
        WriteBatch::SetCount(batch, n);
    }

    static uint64_t Sequence(const WriteBatch* batch) {
        return WriteBatch::Sequence(batch);
    }

    static void SetSequence(WriteBatch* batch, uint64_t seq) {
        WriteBatch::SetSequence(batch, seq);
    }
};

}  // namespace leveldb
