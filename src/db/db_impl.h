#pragma once

#include <mutex>
#include <deque>
#include "db/dbformat.h"
#include "db/log_writer_reader.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "leveldb/db.h"

namespace leveldb {

class DBImpl : public DB {
public:
    DBImpl(const Options& options, const std::string& dbname);
    ~DBImpl() override;

    Status Put(const WriteOptions& o, const Slice& key, const Slice& val) override;
    Status Delete(const WriteOptions& o, const Slice& key) override;
    Status Write(const WriteOptions& options, WriteBatch* updates) override;
    Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;
    Iterator* NewIterator(const ReadOptions& options) override;
    void CompactRange(const Slice* begin, const Slice* end) override;

    // Flush current memtable directly to SSTable (minor compaction)
    Status FlushMemTable();

    // Run major compaction on files across levels
    Status DoCompaction();

private:
    friend class DB;

    Status Recover(VersionEdit* edit);
    Status MakeRoomForWrite(bool force);
    Status WriteLevel0Table(MemTable* mem, VersionEdit* edit);

    const Options options_;
    const InternalKeyComparator internal_comparator_;
    const std::string dbname_;

    std::mutex mutex_;
    MemTable* mem_;
    MemTable* imm_;                // Memtable being flushed to disk
    WritableFile* logfile_;
    log::Writer* log_;
    uint64_t logfile_number_;
    VersionSet* versions_;
    bool shutting_down_;
};

}  // namespace leveldb
