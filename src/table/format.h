#pragma once

#include <cstdint>
#include <string>
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

// BlockHandle is a pointer to the extent of a file that stores a data
// or index block.
class BlockHandle {
public:
    enum { kMaxEncodedLength = 10 + 10 };

    BlockHandle();

    // The offset of the block in the file.
    uint64_t offset() const { return offset_; }
    void set_offset(uint64_t offset) { offset_ = offset; }

    // The size of the stored block
    uint64_t size() const { return size_; }
    void set_size(uint64_t size) { size_ = size; }

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(Slice* input);

private:
    uint64_t offset_;
    uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
class Footer {
public:
    // Encoded length of a Footer.
    // Metaindex_handle (20) + index_handle (20) + padding + magic (8) = 48
    enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

    Footer();

    const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
    void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

    const BlockHandle& index_handle() const { return index_handle_; }
    void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

    void EncodeTo(std::string* dst) const;
    Status DecodeFrom(Slice* input);

private:
    BlockHandle metaindex_handle_;
    BlockHandle index_handle_;
};

// Magic number stored at the end of SSTable file
// Derived from echo http://code.google.com/p/leveldb/ | sha1sum | cut -c 1-16
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1 byte type + 4-byte CRC for table block trailer
static const size_t kBlockTrailerSize = 5;

}  // namespace leveldb
