#include "leveldb/write_batch.h"
#include "db/dbformat.h"
#include "db/memtable.h"
#include "util/coding.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() {
    Clear();
}

void WriteBatch::Clear() {
    rep_.clear();
    rep_.resize(kHeader);
}

int WriteBatch::Count(const WriteBatch* b) {
    return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatch::SetCount(WriteBatch* b, int n) {
    EncodeFixed32(&b->rep_[8], n);
}

uint64_t WriteBatch::Sequence(const WriteBatch* b) {
    return DecodeFixed64(b->rep_.data());
}

void WriteBatch::SetSequence(WriteBatch* b, uint64_t seq) {
    EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
    SetCount(this, Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeValue));
    PutLengthPrefixedSlice(&rep_, key);
    PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
    SetCount(this, Count(this) + 1);
    rep_.push_back(static_cast<char>(kTypeDeletion));
    PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
    SetCount(this, Count(this) + Count(&source));
    assert(source.rep_.size() >= kHeader);
    rep_.append(source.rep_.data() + kHeader, source.rep_.size() - kHeader);
}

Status WriteBatch::Iterate(Handler* handler) const {
    Slice input(rep_);
    if (input.size() < kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
    }

    input.remove_prefix(kHeader);
    Slice key, value;
    int found = 0;
    while (!input.empty()) {
        found++;
        char tag = input[0];
        input.remove_prefix(1);
        switch (tag) {
            case kTypeValue:
                if (GetLengthPrefixedSlice(&input, &key) &&
                    GetLengthPrefixedSlice(&input, &value)) {
                    handler->Put(key, value);
                } else {
                    return Status::Corruption("bad WriteBatch Put");
                }
                break;
            case kTypeDeletion:
                if (GetLengthPrefixedSlice(&input, &key)) {
                    handler->Delete(key);
                } else {
                    return Status::Corruption("bad WriteBatch Delete");
                }
                break;
            default:
                return Status::Corruption("unknown WriteBatch tag");
        }
    }
    if (found != Count(this)) {
        return Status::Corruption("WriteBatch count mismatch");
    } else {
        return Status::OK();
    }
}

}  // namespace leveldb
