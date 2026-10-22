#pragma once

#include <vector>
#include "db/dbformat.h"
#include "db/version_edit.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

namespace log {
class Writer;
}

class WritableFile;
class TableCache;
class VersionSet;
class Version;

// Number of levels in our LSM tree
static const int kNumLevels = 7;

class Version {
public:
    explicit Version(VersionSet* vset);
    ~Version();

    Version(const Version&) = delete;
    Version& operator=(const Version&) = delete;

    void Ref() { ++refs_; }
    void Unref() {
        assert(refs_ > 0);
        --refs_;
        if (refs_ == 0) {
            delete this;
        }
    }

    // Lookup a key across all levels:
    // Level 0 files can overlap, so we check all L0 files from newest to oldest.
    // Level 1..kNumLevels-1 files are disjoint, so we binary search.
    struct GetStats {
        FileMetaData* seek_file;
        int seek_file_level;
    };

    Status Get(const ReadOptions& options, const Slice& internal_key,
               std::string* val, GetStats* stats);

    // List of files at each level
    std::vector<FileMetaData*> files_[kNumLevels];

    // Next/prev in doubly-linked list of versions maintained by VersionSet
    Version* next_;
    Version* prev_;

    // Compaction score and level for best compaction candidate
    double compaction_score_;
    int compaction_level_;

private:
    friend class VersionSet;
    VersionSet* vset_;
    int refs_;
};

class VersionSet {
public:
    VersionSet(const std::string& dbname, const Options* options,
               const InternalKeyComparator* cmp);
    ~VersionSet();

    VersionSet(const VersionSet&) = delete;
    VersionSet& operator=(const VersionSet&) = delete;

    // Apply *edit to the current version to form a new descriptor that is
    // both saved to persistent state and installed as the new current version.
    Status LogAndApply(VersionEdit* edit);

    // Recover the last saved descriptor from persistent storage
    Status Recover();

    Version* current() const { return current_; }
    uint64_t ManifestFileNumber() const { return manifest_file_number_; }
    uint64_t NextFileNumber() { return next_file_number_++; }
    uint64_t NewFileNumber() { return next_file_number_++; }
    void ReuseFileNumber(uint64_t file_number) {
        if (next_file_number_ == file_number + 1) {
            next_file_number_ = file_number;
        }
    }

    uint64_t LastSequence() const { return last_sequence_; }
    void SetLastSequence(uint64_t s) {
        assert(s >= last_sequence_);
        last_sequence_ = s;
    }
    void MarkFileNumberUsed(uint64_t number) {
        if (next_file_number_ <= number) {
            next_file_number_ = number + 1;
        }
    }

    uint64_t LogNumber() const { return log_number_; }
    uint64_t PrevLogNumber() const { return prev_log_number_; }

    const InternalKeyComparator& icmp() const { return icmp_; }
    const Options* options() const { return options_; }
    const std::string& dbname() const { return dbname_; }

    // Pick compaction level and files
    bool NeedsCompaction() const {
        return current_->compaction_score_ >= 1.0;
    }

    void Finalize(Version* v);

private:
    class Builder;

    void AppendVersion(Version* v);

    const std::string dbname_;
    const Options* const options_;
    const InternalKeyComparator icmp_;
    uint64_t next_file_number_;
    uint64_t manifest_file_number_;
    uint64_t last_sequence_;
    uint64_t log_number_;
    uint64_t prev_log_number_;

    WritableFile* descriptor_file_;
    class log::Writer* descriptor_log_;
    Version dummy_versions_;  // Head of circular doubly-linked list
    Version* current_;        // == dummy_versions_.prev_
};

}  // namespace leveldb
