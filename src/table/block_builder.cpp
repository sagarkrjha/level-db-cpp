#include "table/block_builder.h"
#include <algorithm>
#include <cassert>
#include "leveldb/comparator.h"
#include "util/coding.h"

namespace leveldb {

BlockBuilder::BlockBuilder(int block_restart_interval, const Comparator* comparator)
    : block_restart_interval_(block_restart_interval),
      comparator_(comparator ? comparator : BytewiseComparator()),
      counter_(0),
      finished_(false) {
    assert(block_restart_interval_ >= 1);
    restarts_.push_back(0);  // First restart point is at offset 0
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    finished_ = false;
    last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    return (buffer_.size() +                        // Raw data buffer
            restarts_.size() * sizeof(uint32_t) +   // Restart array
            sizeof(uint32_t));                      // Restart array length
}

Slice BlockBuilder::Finish() {
    // Append restart array
    for (size_t i = 0; i < restarts_.size(); i++) {
        PutFixed32(&buffer_, restarts_[i]);
    }
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
    finished_ = true;
    return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    Slice last_key_piece(last_key_);
    assert(!finished_);
    assert(counter_ <= block_restart_interval_);
    assert(buffer_.empty() || comparator_->Compare(key, last_key_piece) > 0);

    size_t shared = 0;
    if (counter_ < block_restart_interval_) {
        // Calculate common prefix length
        const size_t min_len = std::min(last_key_piece.size(), key.size());
        while ((shared < min_len) && (last_key_piece[shared] == key[shared])) {
            shared++;
        }
    } else {
        // Restart compression
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
    }

    const size_t non_shared = key.size() - shared;

    // Add "<shared><non_shared><value_size>" to buffer_
    PutVarint32(&buffer_, shared);
    PutVarint32(&buffer_, non_shared);
    PutVarint32(&buffer_, value.size());

    // Add string delta into buffer_ followed by value
    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    // Update state
    last_key_.resize(shared);
    last_key_.append(key.data() + shared, non_shared);
    assert(Slice(last_key_) == key);
    counter_++;
}

}  // namespace leveldb
