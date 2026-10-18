#pragma once

#include <cstddef>
#include <string>
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

struct Options {
    // Comparator used to define the order of keys in the table.
    // Default: a comparator that uses lexicographic byte-wise ordering
    const class Comparator* comparator = nullptr;

    // If true, the database will be created if it is missing.
    // Default: false
    bool create_if_missing = false;

    // If true, an error is raised if the database already exists.
    // Default: false
    bool error_if_exists = false;

    // Approximate size of user data packed per block (in bytes).
    // Default: 4K
    size_t block_size = 4096;

    // Number of keys between restart points for delta encoding of keys.
    // Default: 16
    int block_restart_interval = 16;

    // Maximum file size of SSTables (usually 2MB)
    size_t max_file_size = 2 * 1024 * 1024;

    // Amount of data to build up in memory before converting to a sorted on-disk file.
    // Default: 4MB
    size_t write_buffer_size = 4 * 1024 * 1024;

    // Filter policy (e.g. BloomFilter). If nullptr, no filter is used.
    const class FilterPolicy* filter_policy = nullptr;

    Options();
};

struct ReadOptions {
    // If true, all data read from underlying storage will be
    // verified against corresponding checksums.
    bool verify_checksums = false;

    // Should the data read for this query be cached in memory?
    bool fill_cache = true;
};

struct WriteOptions {
    // If true, the write will be flushed from the operating system
    // buffer cache (sync-to-disk) before the write is considered complete.
    bool sync = false;
};

}  // namespace leveldb
