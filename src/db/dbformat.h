#pragma once

#include <cstdint>
#include <string>
#include "leveldb/comparator.h"
#include "leveldb/slice.h"
#include "util/coding.h"

namespace leveldb {

// Value types encoded as the last byte of an InternalKey
enum ValueType {
    kTypeDeletion = 0x0,
    kTypeValue = 0x1
};

// kValueTypeForSeek defines the ValueType that should be passed when
// constructing an InternalKey for seeking to a particular sequence number (descending order)
static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;
// We leave eight bits empty at the bottom for ValueType
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

struct ParsedInternalKey {
    Slice user_key;
    SequenceNumber sequence;
    ValueType type;

    ParsedInternalKey() : sequence(0), type(kTypeValue) {}
    ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
        : user_key(u), sequence(seq), type(t) {}
};

// Return the length of the encoding of "key".
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
    return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key". Returns true on success.
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// Returns the user key portion of an internal key.
inline Slice ExtractUserKey(const Slice& internal_key) {
    assert(internal_key.size() >= 8);
    return Slice(internal_key.data(), internal_key.size() - 8);
}

class InternalKey;

// InternalKey comparator: orders by user_key ascending, then sequence number descending
class InternalKeyComparator : public Comparator {
private:
    const Comparator* user_comparator_;

public:
    explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
    const char* Name() const override;
    int Compare(const Slice& a, const Slice& b) const override;
    void FindShortestSeparator(std::string* start, const Slice& limit) const override;
    void FindShortSuccessor(std::string* key) const override;

    const Comparator* user_comparator() const { return user_comparator_; }
    int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Modules that operate on keys use InternalKey
class InternalKey {
private:
    std::string rep_;

public:
    InternalKey() {}  // Leaves rep_ empty
    InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
        AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
    }

    void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
    Slice Encode() const {
        assert(!rep_.empty());
        return rep_;
    }

    Slice user_key() const { return ExtractUserKey(rep_); }
    void SetFrom(const ParsedInternalKey& p) {
        rep_.clear();
        AppendInternalKey(&rep_, p);
    }
    void Clear() { rep_.clear(); }
    size_t size() const { return rep_.size(); }
};

// Helper class for easy extraction of the user key
class LookupKey {
private:
    // We construct a char buffer:
    // [varint32(klength)] [user_key] [tag 8-byte]
    const char* start_;
    const char* kstart_;
    const char* end_;
    char space_[200];  // Avoid allocation for short keys

public:
    LookupKey(const Slice& user_key, SequenceNumber sequence);
    ~LookupKey();

    Slice memtable_key() const { return Slice(start_, end_ - start_); }
    Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }
    Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }
};

inline int InternalKeyComparator::Compare(const InternalKey& a, const InternalKey& b) const {
    return Compare(a.Encode(), b.Encode());
}

}  // namespace leveldb
