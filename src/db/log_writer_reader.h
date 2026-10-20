#pragma once

#include <cstdint>
#include <string>
#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WritableFile {
public:
    virtual ~WritableFile() = default;
    virtual Status Append(const Slice& data) = 0;
    virtual Status Close() = 0;
    virtual Status Flush() = 0;
    virtual Status Sync() = 0;
};

class SequentialFile {
public:
    virtual ~SequentialFile() = default;
    virtual Status Read(size_t n, Slice* result, char* scratch) = 0;
    virtual Status Skip(uint64_t n) = 0;
};

class RandomAccessFile {
public:
    virtual ~RandomAccessFile() = default;
    virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

namespace log {

class Writer {
public:
    explicit Writer(WritableFile* dest);
    Writer(WritableFile* dest, uint64_t dest_length);
    ~Writer() = default;

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Status AddRecord(const Slice& slice);

private:
    Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

    WritableFile* dest_;
    int block_offset_;  // Current offset in block

    // crc32c values for all supported record types
    uint32_t type_crc_[kMaxRecordType + 1];
};

class Reader {
public:
    class Reporter {
    public:
        virtual ~Reporter() = default;
        virtual void Corruption(size_t bytes, const Status& status) = 0;
    };

    Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset);
    ~Reader() = default;

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool ReadRecord(Slice* record, std::string* scratch);
    uint64_t LastRecordOffset() const { return last_record_offset_; }

private:
    enum {
        kEof = kMaxRecordType + 1,
        // Returned whenever we find an invalid physical record
        kBadRecord = kMaxRecordType + 2
    };

    // Skips all blocks that come before "initial_offset_".
    bool SkipToInitialBlock();

    unsigned int ReadPhysicalRecord(Slice* result);
    void ReportCorruption(uint64_t bytes, const char* reason);
    void ReportDrop(uint64_t bytes, const Status& reason);

    SequentialFile* const file_;
    Reporter* const reporter_;
    bool const checksum_;
    char* const backing_store_;
    Slice buffer_;
    bool eof_;

    uint64_t last_record_offset_;
    uint64_t end_of_buffer_offset_;
    uint64_t const initial_offset_;
    bool resyncing_;
};

}  // namespace log
}  // namespace leveldb
