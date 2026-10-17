# LevelDB C++ Implementation

A complete, clean, end-to-end modern C++ (C++20) implementation of the Log-Structured Merge-tree (LSM-tree) storage engine.

---

## 🌟 Acknowledgement & Shoutout

> [!NOTE]
> **Massive shoutout to Google's [LevelDB](https://github.com/google/leveldb)** created by **Sanjay Ghemawat** and **Jeff Dean**.
> Their elegant LSM-tree architecture, write-ahead logging format, block prefix-compression designs, and two-level table indexing served as the direct inspiration and foundational blueprint for this project.

---

## 🏗️ Architecture & Component Overview

```
                      +-------------------+
                      |   Client Writes   |
                      +---------+---------+
                                |
               +----------------+----------------+
               |                                 |
               v                                 v
        +--------------+                 +---------------+
        | Write-Ahead  |                 |    MemTable   |
        |  Log (WAL)   |                 |  (SkipList)   |
        +--------------+                 +-------+-------+
                                                 | (when write_buffer_size reached)
                                                 v
                                         +---------------+
                                         | Immutable     |
                                         | MemTable      |
                                         +-------+-------+
                                                 | (Minor Compaction)
                                                 v
    +------------------+                 +---------------+
    | Manifest /       |<----------------| SSTable L0    |
    | VersionEdit /    |                 +-------+-------+
    | VersionSet       |                         | (Major Compaction)
    +------------------+                         v
                                         +---------------+
                                         | SSTable L1..Ln|
                                         +---------------+
```

### 1. Storage & Memory Pipeline
* **[Slice](include/leveldb/slice.h)**: Zero-copy read buffer views.
* **[Arena](src/util/arena.h)** & **[SkipList](src/db/skiplist.h)**: Lock-free lockless read skiplist using atomic pointer operations and memory arena allocation.
* **[InternalKey](src/db/dbformat.h)**: Multiplexed user key + 56-bit sequence number + 8-bit `ValueType` (`kTypeValue` or `kTypeDeletion`).
* **[MemTable](src/db/memtable.h)**: In-memory sorted buffer. Provides fast point lookups and range scans.
* **[WriteBatch](src/db/write_batch.cpp)**: Atomic update batch supporting Put/Delete with linear sequence allocation.

### 2. Write-Ahead Log (WAL)
* **[LogWriter & LogReader](src/db/log_writer_reader.h)**: 32 KB chunked physical record framing (`kFullType`, `kFirstType`, `kMiddleType`, `kLastType`) with CRC32C integrity masking and recovery resynchronization.

### 3. SSTable (Sorted String Table Format)
* **[BlockBuilder](src/table/block_builder.h) & [Block](src/table/block.h)**: Prefix compression (shared / non-shared byte encoding) with configurable restart intervals (default: 16) and binary search across restart points.
* **[Bloom Filter](src/table/filter_block.cpp)**: Double-hashing Bloom Filter (`k = bits_per_key * ln(2)`) to eliminate disk seeks for absent keys.
* **[TableBuilder](src/table/table_builder.h) & [Table](src/table/table.h)**: Complete LevelDB on-disk SSTable layout: Data Blocks -> Filter Block -> MetaIndex Block -> Index Block -> 48-byte fixed Footer with magic `0xdb4775248b80fb57`.

### 4. Compaction & Version Management
* **[VersionEdit](src/db/version_edit.h)**: Atomic delta journal log (added SSTable files, deleted SSTable files, next file ID, last sequence number).
* **[VersionSet](src/db/version_set.h)**: MVCC version chain management, MANIFEST file logger, and `CURRENT` pointer management.
* **Compaction Engine**:
  * **Minor Compaction**: Flushes immutable in-memory MemTable directly into Level 0 SSTables.
  * **Major Compaction**: Detects score threshold exceedances (`L0 >= 4` files, or size limits on levels L1..L6), merges disjoint/overlapping tables using `MergingIterator`, deduplicates shadowed sequence numbers, purges deleted tombstones, and emits new sorted SSTable extents.

---

## 🛠️ Building & Testing

### Prerequisites
* GCC or Clang with C++20 support
* CMake 3.20+

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
ctest --output-on-failure
```

---

## 💻 Code Example

```cpp
#include <iostream>
#include "leveldb/db.h"

int main() {
    leveldb::DB* db = nullptr;
    leveldb::Options options;
    options.create_if_missing = true;

    leveldb::Status s = leveldb::DB::Open(options, "/tmp/demo_db", &db);
    if (!s.ok()) {
        std::cerr << s.ToString() << std::endl;
        return 1;
    }

    // 1. Put
    db->Put(leveldb::WriteOptions(), "user:101", "Alice");

    // 2. Get
    std::string val;
    db->Get(leveldb::ReadOptions(), "user:101", &val);
    std::cout << "Read value: " << val << std::endl;

    // 3. Batch write
    leveldb::WriteBatch batch;
    batch.Put("user:102", "Bob");
    batch.Delete("user:101");
    db->Write(leveldb::WriteOptions(), &batch);

    // 4. Iterate
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::cout << it->key().ToString() << " => " << it->value().ToString() << std::endl;
    }
    delete it;

    delete db;
    return 0;
}
```
