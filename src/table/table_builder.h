#pragma once

#include "leveldb/options.h"
#include "leveldb/status.h"
#include "table/format.h"

namespace leveldb {

class WritableFile;

class TableBuilder {
public:
    TableBuilder(const Options& options, WritableFile* file);
    ~TableBuilder();

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // Add key,value to the table being constructed.
    // REQUIRES: key is after any previously added key
    // REQUIRES: table is not abandoned or finished
    void Add(const Slice& key, const Slice& value);

    // Advanced block flush
    void Flush();

    // Finish building the table. Stops using the file passed to the
    // constructor after this function returns.
    Status Finish();

    // Abandon building this table.
    void Abandon();

    // Number of calls to Add() so far.
    uint64_t NumEntries() const;

    // Size of the file generated so far.
    uint64_t FileSize() const;

    // Status of table builder.
    Status status() const;

private:
    bool ok() const { return status().ok(); }
    void WriteBlock(class BlockBuilder* block, BlockHandle* handle);
    void WriteRawBlock(const Slice& data, BlockHandle* handle);

    struct Rep;
    Rep* rep_;
};

}  // namespace leveldb
