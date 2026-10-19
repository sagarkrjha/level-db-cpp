#include "db/memtable.h"
#include "util/coding.h"

namespace leveldb {

static Slice GetLengthPrefixedSlice(const char* data) {
    uint32_t len;
    const char* p = data;
    p = GetVarint32Ptr(p, p + 5, &len);
    return Slice(p, len);
}

MemTable::MemTable(const InternalKeyComparator& cmp)
    : comparator_(cmp), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() {
    assert(refs_ == 0);
}

size_t MemTable::ApproximateMemoryUsage() {
    return arena_.MemoryUsage();
}

int MemTable::KeyComparator::operator()(const char* aptr, const char* bptr) const {
    // Internal keys are encoded as length-prefixed strings.
    Slice a = GetLengthPrefixedSlice(aptr);
    Slice b = GetLengthPrefixedSlice(bptr);
    return comparator.Compare(a, b);
}

// Encode a suitable internal key format into a contiguous buffer
// Format:
//   key_length: varint32 of internal_key.size()
//   key bytes:  user_key
//   tag:        (seq << 8) | type (8 bytes)
//   val_length: varint32 of value.size()
//   val bytes:  value
void MemTable::Add(SequenceNumber s, ValueType type,
                   const Slice& key, const Slice& value) {
    size_t key_size = key.size();
    size_t val_size = value.size();
    size_t internal_key_size = key_size + 8;
    const size_t encoded_len =
        VarintLength(internal_key_size) + internal_key_size +
        VarintLength(val_size) + val_size;
    char* buf = arena_.Allocate(encoded_len);
    char* p = EncodeVarint32(buf, internal_key_size);
    std::memcpy(p, key.data(), key_size);
    p += key_size;
    EncodeFixed64(p, (s << 8) | type);
    p += 8;
    p = EncodeVarint32(p, val_size);
    std::memcpy(p, value.data(), val_size);
    table_.Insert(buf);
}

bool MemTable::Get(const Slice& user_key, SequenceNumber seq,
                   std::string* value, Status* s) {
    // Build a lookup key
    // We encode a lookup key as:
    // length-prefixed internal key with seq and kValueTypeForSeek
    size_t key_size = user_key.size();
    size_t internal_key_size = key_size + 8;
    std::string lookup_key;
    PutVarint32(&lookup_key, internal_key_size);
    lookup_key.append(user_key.data(), key_size);
    PutFixed64(&lookup_key, (seq << 8) | kValueTypeForSeek);

    Table::Iterator iter(&table_);
    iter.Seek(lookup_key.data());
    if (iter.Valid()) {
        // Entry format: [varint32 klen][user_key][tag 8B][varint32 vlen][val]
        const char* entry = iter.key();
        uint32_t key_length;
        const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
        if (comparator_.comparator.user_comparator()->Compare(
                Slice(key_ptr, key_length - 8), user_key) == 0) {
            // Correct user key
            const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
            switch (static_cast<ValueType>(tag & 0xff)) {
                case kTypeValue: {
                    Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
                    value->assign(v.data(), v.size());
                    return true;
                }
                case kTypeDeletion:
                    *s = Status::NotFound(Slice());
                    return true;
            }
        }
    }
    return false;
}

class MemTableIterator : public Iterator {
public:
    explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

    bool Valid() const override { return iter_.Valid(); }
    void Seek(const Slice& k) override {
        // k is an internal key; we need to wrap it into length-prefixed
        std::string encoded;
        PutVarint32(&encoded, k.size());
        encoded.append(k.data(), k.size());
        iter_.Seek(encoded.data());
    }
    void SeekToFirst() override { iter_.SeekToFirst(); }
    void SeekToLast() override { iter_.SeekToLast(); }
    void Next() override { iter_.Next(); }
    void Prev() override { iter_.Prev(); }

    Slice key() const override {
        return GetLengthPrefixedSlice(iter_.key());
    }
    Slice value() const override {
        Slice key_slice = GetLengthPrefixedSlice(iter_.key());
        return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
    }
    Status status() const override { return Status::OK(); }

private:
    MemTable::Table::Iterator iter_;
};

Iterator* MemTable::NewIterator() {
    return new MemTableIterator(&table_);
}

}  // namespace leveldb
