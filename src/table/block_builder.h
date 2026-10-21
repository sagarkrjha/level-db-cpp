#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "leveldb/slice.h"

namespace leveldb {

struct Options;

class Comparator;

class BlockBuilder {
public:
    explicit BlockBuilder(int block_restart_interval, const Comparator* comparator = nullptr);

    BlockBuilder(const BlockBuilder&) = delete;
    BlockBuilder& operator=(const BlockBuilder&) = delete;

    // Reset the contents as if the BlockBuilder was just constructed.
    void Reset();

    // REQUIRES: Finish() has not been called since the last call to Reset().
    // REQUIRES: key is larger than any previously added key
    void Add(const Slice& key, const Slice& value);

    // Finish building the block and return a slice that refers to the
    // block contents. The returned slice will remain valid for the
    // lifetime of this builder or until Reset() is called.
    Slice Finish();

    // Returns an estimate of the current size of the block in bytes.
    size_t CurrentSizeEstimate() const;

    // Return true iff no entries have been added since the last Reset()
    bool empty() const { return buffer_.empty(); }

private:
    int const block_restart_interval_;
    const Comparator* const comparator_;
    std::string buffer_;              // Destination buffer
    std::vector<uint32_t> restarts_;  // Restart points
    int counter_;                     // Number of entries emitted since restart
    bool finished_;                   // Has Finish() been called?
    std::string last_key_;
};

}  // namespace leveldb
