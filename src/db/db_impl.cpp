#include "db/db_impl.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include "db/filename.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "table/merger.h"
#include "table/table.h"
#include "table/table_builder.h"
#include "util/env_posix.h"

namespace leveldb {

// Helper class for WriteBatch application to MemTable
class MemTableInserter : public WriteBatch::Handler {
public:
    SequenceNumber sequence_;
    MemTable* mem_;

    void Put(const Slice& key, const Slice& value) override {
        mem_->Add(sequence_, kTypeValue, key, value);
        sequence_++;
    }
    void Delete(const Slice& key) override {
        mem_->Add(sequence_, kTypeDeletion, key, Slice());
        sequence_++;
    }
};

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : options_(raw_options),
      internal_comparator_(raw_options.comparator),
      dbname_(dbname),
      mem_(nullptr),
      imm_(nullptr),
      logfile_(nullptr),
      log_(nullptr),
      logfile_number_(0),
      versions_(new VersionSet(dbname, &options_, &internal_comparator_)),
      shutting_down_(false) {
    mem_ = new MemTable(internal_comparator_);
    mem_->Ref();
}

DBImpl::~DBImpl() {
    shutting_down_ = true;
    if (log_ != nullptr) {
        delete log_;
        delete logfile_;
    }
    if (mem_ != nullptr) mem_->Unref();
    if (imm_ != nullptr) imm_->Unref();
    delete versions_;
}

Status DBImpl::Recover(VersionEdit* edit) {
    Env::CreateDir(dbname_);

    Status s;
    if (!Env::FileExists(CurrentFileName(dbname_))) {
        if (options_.create_if_missing) {
            edit->SetComparatorName(internal_comparator_.user_comparator()->Name());
            edit->SetLogNumber(0);
            edit->SetNextFile(2);
            edit->SetLastSequence(0);
            s = versions_->LogAndApply(edit);
        } else {
            return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
        }
    } else {
        if (options_.error_if_exists) {
            return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
        }
        s = versions_->Recover();
    }
    return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit) {
    const uint64_t file_number = versions_->NewFileNumber();
    std::string fname = TableFileName(dbname_, file_number);

    WritableFile* file = nullptr;
    Status s = Env::NewWritableFile(fname, &file);
    if (!s.ok()) return s;

    Options table_options = options_;
    table_options.comparator = &internal_comparator_;
    TableBuilder* builder = new TableBuilder(table_options, file);
    Iterator* iter = mem->NewIterator();
    iter->SeekToFirst();

    FileMetaData meta;
    meta.number = file_number;

    if (iter->Valid()) {
        meta.smallest.DecodeFrom(iter->key());
        for (; iter->Valid(); iter->Next()) {
            meta.largest.DecodeFrom(iter->key());
            builder->Add(iter->key(), iter->value());
        }
    }

    s = builder->Finish();
    if (s.ok()) {
        meta.file_size = builder->FileSize();
        assert(meta.file_size > 0);
        s = file->Sync();
    }
    if (s.ok()) {
        s = file->Close();
    }
    delete iter;
    delete builder;
    delete file;

    if (s.ok()) {
        // Minor compaction writes straight to Level 0
        edit->AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
    }
    return s;
}

Status DBImpl::FlushMemTable() {
    std::lock_guard<std::mutex> l(mutex_);
    if (mem_ == nullptr || mem_->ApproximateMemoryUsage() == 0) {
        return Status::OK();
    }

    // Switch active memtable to immutable
    imm_ = mem_;
    mem_ = new MemTable(internal_comparator_);
    mem_->Ref();

    VersionEdit edit;
    Status s = WriteLevel0Table(imm_, &edit);
    if (s.ok()) {
        edit.SetPrevLogNumber(0);
        edit.SetLogNumber(logfile_number_);
        s = versions_->LogAndApply(&edit);
    }
    imm_->Unref();
    imm_ = nullptr;

    // Check if compaction is needed
    if (s.ok() && versions_->NeedsCompaction()) {
        DoCompaction();
    }
    return s;
}

Status DBImpl::DoCompaction() {
    if (!versions_->NeedsCompaction()) {
        return Status::OK();
    }

    Version* current = versions_->current();
    int level = current->compaction_level_;
    if (level < 0 || level >= kNumLevels - 1) {
        return Status::OK();
    }

    if (current->files_[level].empty()) {
        return Status::OK();
    }

    // Major compaction: Pick files from `level` and overlapping files from `level + 1`
    std::vector<FileMetaData*> level_files = current->files_[level];
    std::vector<FileMetaData*> next_level_files = current->files_[level + 1];

    // Compute key range for selected level files
    InternalKey smallest = level_files[0]->smallest;
    InternalKey largest = level_files[0]->largest;
    for (size_t i = 1; i < level_files.size(); i++) {
        if (internal_comparator_.Compare(level_files[i]->smallest, smallest) < 0) {
            smallest = level_files[i]->smallest;
        }
        if (internal_comparator_.Compare(level_files[i]->largest, largest) > 0) {
            largest = level_files[i]->largest;
        }
    }

    // Find overlapping files at level + 1
    std::vector<FileMetaData*> overlaps;
    const Comparator* ucmp = internal_comparator_.user_comparator();
    for (size_t i = 0; i < next_level_files.size(); i++) {
        FileMetaData* f = next_level_files[i];
        if (ucmp->Compare(f->largest.user_key(), smallest.user_key()) >= 0 &&
            ucmp->Compare(f->smallest.user_key(), largest.user_key()) <= 0) {
            overlaps.push_back(f);
        }
    }

    // Open iterators across both levels
    std::vector<Iterator*> iters;
    std::vector<RandomAccessFile*> open_files;
    std::vector<Table*> open_tables;

    auto add_files_iters = [&](const std::vector<FileMetaData*>& fvec) {
        for (auto* f : fvec) {
            std::string path = TableFileName(dbname_, f->number);
            RandomAccessFile* rfile = nullptr;
            if (Env::NewRandomAccessFile(path, &rfile).ok()) {
                Table* tbl = nullptr;
                Options table_options = options_;
                table_options.comparator = &internal_comparator_;
                if (Table::Open(table_options, rfile, f->file_size, &tbl).ok()) {
                    open_files.push_back(rfile);
                    open_tables.push_back(tbl);
                    iters.push_back(tbl->NewIterator(ReadOptions()));
                } else {
                    delete rfile;
                }
            }
        }
    };

    add_files_iters(level_files);
    add_files_iters(overlaps);

    if (iters.empty()) {
        return Status::OK();
    }

    std::unique_ptr<Iterator> merge_iter(
        NewMergingIterator(&internal_comparator_, iters.data(), static_cast<int>(iters.size())));
    merge_iter->SeekToFirst();

    // Compact down to level + 1
    VersionEdit edit;
    // Mark old files as deleted
    for (auto* f : level_files) {
        edit.DeleteFile(level, f->number);
    }
    for (auto* f : overlaps) {
        edit.DeleteFile(level + 1, f->number);
    }

    TableBuilder* builder = nullptr;
    WritableFile* outfile = nullptr;
    FileMetaData meta;
    std::string current_user_key;
    bool has_current_user_key = false;

    auto finish_table = [&]() {
        if (builder != nullptr) {
            builder->Finish();
            meta.file_size = builder->FileSize();
            outfile->Sync();
            outfile->Close();
            delete builder;
            delete outfile;
            builder = nullptr;
            outfile = nullptr;
            edit.AddFile(level + 1, meta.number, meta.file_size, meta.smallest, meta.largest);
        }
    };

    while (merge_iter->Valid()) {
        Slice key = merge_iter->key();
        ParsedInternalKey ikey;
        ParseInternalKey(key, &ikey);

        // Deduplication rule: drop obsolete sequence versions of user keys
        bool drop = false;
        if (!has_current_user_key ||
            ucmp->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
            current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
            has_current_user_key = true;
        } else {
            // Shadowed by newer sequence number earlier
            drop = true;
        }

        if (ikey.type == kTypeDeletion && level + 1 >= kNumLevels - 1) {
            // Tombstone can be safely dropped if at deepest level
            drop = true;
        }

        if (!drop) {
            if (builder == nullptr) {
                meta.number = versions_->NewFileNumber();
                std::string fname = TableFileName(dbname_, meta.number);
                Env::NewWritableFile(fname, &outfile);
                Options table_options = options_;
                table_options.comparator = &internal_comparator_;
                builder = new TableBuilder(table_options, outfile);
                meta.smallest.DecodeFrom(key);
            }
            meta.largest.DecodeFrom(key);
            builder->Add(key, merge_iter->value());

            if (builder->FileSize() >= options_.max_file_size) {
                finish_table();
            }
        }
        merge_iter->Next();
    }
    finish_table();

    // Clean up input tables
    for (auto* tbl : open_tables) delete tbl;
    for (auto* rfile : open_files) delete rfile;

    // Apply edits
    Status s = versions_->LogAndApply(&edit);

    // Delete obsolete table files from disk
    for (auto* f : level_files) {
        Env::DeleteFile(TableFileName(dbname_, f->number));
    }
    for (auto* f : overlaps) {
        Env::DeleteFile(TableFileName(dbname_, f->number));
    }

    return s;
}

Status DBImpl::MakeRoomForWrite(bool /*force*/) {
    while (true) {
        if (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
            // There is room in current memtable
            return Status::OK();
        } else if (imm_ != nullptr) {
            // Immutable memtable flush is pending
            return Status::IOError("Compaction pending");
        } else {
            // Flush memtable to Level 0
            imm_ = mem_;
            mem_ = new MemTable(internal_comparator_);
            mem_->Ref();

            VersionEdit edit;
            Status s = WriteLevel0Table(imm_, &edit);
            if (!s.ok()) return s;
            s = versions_->LogAndApply(&edit);
            if (!s.ok()) return s;

            imm_->Unref();
            imm_ = nullptr;
        }
    }
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
    std::lock_guard<std::mutex> l(mutex_);
    Status status = MakeRoomForWrite(false);
    if (!status.ok()) return status;

    uint64_t last_sequence = versions_->LastSequence();
    WriteBatch* updates = my_batch;
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);

    // Write to WAL
    Slice record = updates->Data();
    status = log_->AddRecord(record);
    if (status.ok() && options.sync) {
        status = logfile_->Sync();
    }

    if (status.ok()) {
        MemTableInserter inserter;
        inserter.sequence_ = WriteBatchInternal::Sequence(updates);
        inserter.mem_ = mem_;
        status = updates->Iterate(&inserter);
    }

    if (status.ok()) {
        versions_->SetLastSequence(last_sequence);
    }
    return status;
}

Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
    WriteBatch batch;
    batch.Put(key, val);
    return Write(o, &batch);
}

Status DBImpl::Delete(const WriteOptions& o, const Slice& key) {
    WriteBatch batch;
    batch.Delete(key);
    return Write(o, &batch);
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
    std::lock_guard<std::mutex> l(mutex_);
    SequenceNumber snapshot = versions_->LastSequence();

    MemTable* mem = mem_;
    MemTable* imm = imm_;
    Version* current = versions_->current();
    mem->Ref();
    if (imm != nullptr) imm->Ref();
    current->Ref();

    Status s;
    // 1. Check current memtable
    bool found = mem->Get(key, snapshot, value, &s);
    if (!found && imm != nullptr) {
        // 2. Check immutable memtable
        found = imm->Get(key, snapshot, value, &s);
    }
    if (!found) {
        // 3. Check version/SSTable files
        LookupKey lkey(key, snapshot);
        s = current->Get(options, lkey.internal_key(), value, nullptr);
    }

    mem->Unref();
    if (imm != nullptr) imm->Unref();
    current->Unref();

    return s;
}

// DBIter wraps internal keys to provide clean user keys to the caller
class DBIterator : public Iterator {
public:
    DBIterator(Iterator* iter, const Comparator* user_cmp, SequenceNumber seq)
        : iter_(iter), user_cmp_(user_cmp), sequence_(seq), valid_(false) {}
    ~DBIterator() override { delete iter_; }

    bool Valid() const override { return valid_; }
    Slice key() const override {
        assert(Valid());
        return saved_key_;
    }
    Slice value() const override {
        assert(Valid());
        return iter_->value();
    }
    Status status() const override { return iter_->status(); }

    void SeekToFirst() override {
        iter_->SeekToFirst();
        FindNextUserEntry(false);
    }
    void SeekToLast() override {
        iter_->SeekToLast();
        // Simplified backward search
        FindPrevUserEntry();
    }
    void Seek(const Slice& target) override {
        InternalKey ikey(target, sequence_, kValueTypeForSeek);
        iter_->Seek(ikey.Encode());
        FindNextUserEntry(false);
    }
    void Next() override {
        assert(Valid());
        FindNextUserEntry(true);
    }
    void Prev() override {
        assert(Valid());
        FindPrevUserEntry();
    }

private:
    void FindNextUserEntry(bool skipping) {
        while (iter_->Valid()) {
            ParsedInternalKey ikey;
            if (ParseInternalKey(iter_->key(), &ikey) && ikey.sequence <= sequence_) {
                if (skipping && user_cmp_->Compare(ikey.user_key, Slice(saved_key_)) == 0) {
                    // Skip older versions of the same user key
                } else {
                    saved_key_.assign(ikey.user_key.data(), ikey.user_key.size());
                    if (ikey.type == kTypeValue) {
                        valid_ = true;
                        return;
                    }
                    // It's a deletion tombstone; skip to next user key
                    skipping = true;
                }
            }
            iter_->Next();
        }
        valid_ = false;
    }

    void FindPrevUserEntry() {
        while (iter_->Valid()) {
            ParsedInternalKey ikey;
            if (ParseInternalKey(iter_->key(), &ikey) && ikey.sequence <= sequence_ && ikey.type == kTypeValue) {
                saved_key_.assign(ikey.user_key.data(), ikey.user_key.size());
                valid_ = true;
                return;
            }
            iter_->Prev();
        }
        valid_ = false;
    }

    Iterator* iter_;
    const Comparator* user_cmp_;
    SequenceNumber sequence_;
    bool valid_;
    std::string saved_key_;
};

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<Iterator*> list;
    list.push_back(mem_->NewIterator());
    if (imm_ != nullptr) {
        list.push_back(imm_->NewIterator());
    }

    // Add SSTable iterators
    Version* current = versions_->current();
    for (int level = 0; level < kNumLevels; level++) {
        for (auto* f : current->files_[level]) {
            std::string path = TableFileName(dbname_, f->number);
            RandomAccessFile* rfile = nullptr;
            if (Env::NewRandomAccessFile(path, &rfile).ok()) {
                Table* tbl = nullptr;
                Options table_options = options_;
                table_options.comparator = &internal_comparator_;
                if (Table::Open(table_options, rfile, f->file_size, &tbl).ok()) {
                    list.push_back(tbl->NewIterator(options));
                }
            }
        }
    }

    Iterator* internal_iter = NewMergingIterator(&internal_comparator_, list.data(), static_cast<int>(list.size()));
    return new DBIterator(internal_iter, internal_comparator_.user_comparator(), versions_->LastSequence());
}

void DBImpl::CompactRange(const Slice* /*begin*/, const Slice* /*end*/) {
    FlushMemTable();
    DoCompaction();
}

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
    *dbptr = nullptr;
    DBImpl* impl = new DBImpl(options, dbname);
    VersionEdit edit;
    Status s = impl->Recover(&edit);
    if (s.ok()) {
        uint64_t new_log_number = impl->versions_->NewFileNumber();
        WritableFile* lfile = nullptr;
        s = Env::NewWritableFile(LogFileName(dbname, new_log_number), &lfile);
        if (s.ok()) {
            edit.SetLogNumber(new_log_number);
            impl->logfile_ = lfile;
            impl->logfile_number_ = new_log_number;
            impl->log_ = new log::Writer(lfile);
            s = impl->versions_->LogAndApply(&edit);
        }
    }
    if (s.ok()) {
        *dbptr = impl;
    } else {
        delete impl;
    }
    return s;
}

Status DestroyDB(const std::string& dbname, const Options& /*options*/) {
    std::filesystem::remove_all(dbname);
    return Status::OK();
}

}  // namespace leveldb
