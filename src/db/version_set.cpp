#include "db/version_set.h"
#include <algorithm>
#include <fstream>
#include "db/filename.h"
#include "db/log_writer_reader.h"
#include "table/table.h"
#include "util/env_posix.h"

namespace leveldb {

// Maximum bytes of files in each level
static double MaxBytesForLevel(int level) {
    // Result for level-0 is not used (we use number of files instead)
    double result = 10. * 1048576.0;  // 10MB
    while (level > 1) {
        result *= 10;
        level--;
    }
    return result;
}

static uint64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
    uint64_t sum = 0;
    for (size_t i = 0; i < files.size(); i++) {
        sum += files[i]->file_size;
    }
    return sum;
}

Version::Version(VersionSet* vset)
    : next_(this),
      prev_(this),
      compaction_score_(-1),
      compaction_level_(-1),
      vset_(vset),
      refs_(0) {}

Version::~Version() {
    assert(refs_ == 0);
    // Remove from linked list
    prev_->next_ = next_;
    next_->prev_ = prev_;

    for (int level = 0; level < kNumLevels; level++) {
        for (size_t i = 0; i < files_[level].size(); i++) {
            FileMetaData* f = files_[level][i];
            assert(f->refs > 0);
            f->refs--;
            if (f->refs <= 0) {
                delete f;
            }
        }
    }
}

static void SaveValue(void* arg, const Slice& /*ikey*/, const Slice& v) {
    std::pair<Status, std::string>* result =
        reinterpret_cast<std::pair<Status, std::string>*>(arg);
    result->first = Status::OK();
    result->second.assign(v.data(), v.size());
}

Status Version::Get(const ReadOptions& options, const Slice& k,
                    std::string* val, GetStats* stats) {
    Slice user_key = ExtractUserKey(k);
    const Comparator* ucmp = vset_->icmp().user_comparator();
    Status s = Status::NotFound(Slice());

    std::vector<FileMetaData*> tmp;
    FileMetaData* tmp2;

    for (int level = 0; level < kNumLevels; level++) {
        size_t num_files = files_[level].size();
        if (num_files == 0) continue;

        FileMetaData* const* files = &files_[level][0];
        if (level == 0) {
            // Level-0 files may overlap each other. Find all files that
            // overlap user_key and sort them by file number descending (newest first).
            tmp.reserve(num_files);
            for (size_t i = 0; i < num_files; i++) {
                FileMetaData* f = files[i];
                if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
                    ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
                    tmp.push_back(f);
                }
            }
            if (tmp.empty()) continue;

            std::sort(tmp.begin(), tmp.end(), [](FileMetaData* a, FileMetaData* b) {
                return a->number > b->number;
            });
            files = &tmp[0];
            num_files = tmp.size();
        } else {
            // Binary search to find file that may contain user_key
            size_t l = 0;
            size_t r = num_files;
            while (l < r) {
                size_t mid = (l + r) / 2;
                if (ucmp->Compare(files[mid]->largest.user_key(), user_key) < 0) {
                    l = mid + 1;
                } else {
                    r = mid;
                }
            }
            if (l >= num_files) continue;
            tmp2 = files[l];
            if (ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0) {
                continue;
            }
            files = &tmp2;
            num_files = 1;
        }

        for (size_t i = 0; i < num_files; ++i) {
            FileMetaData* f = files[i];
            std::string fname = TableFileName(vset_->dbname(), f->number);
            RandomAccessFile* rfile = nullptr;
            s = Env::NewRandomAccessFile(fname, &rfile);
            if (!s.ok()) return s;

            Table* table = nullptr;
            Options table_options = *vset_->options();
            table_options.comparator = &vset_->icmp();
            s = Table::Open(table_options, rfile, f->file_size, &table);
            if (!s.ok()) {
                delete rfile;
                return s;
            }

            std::pair<Status, std::string> lookup_result(Status::NotFound(Slice()), "");
            table->InternalGet(options, k, &lookup_result, &SaveValue);
            delete table;
            delete rfile;

            if (lookup_result.first.ok()) {
                *val = std::move(lookup_result.second);
                return Status::OK();
            }
        }
    }
    return Status::NotFound(Slice());
}

// Builder helps apply a VersionEdit onto an existing Version
class VersionSet::Builder {
private:
    struct BySmallestKey {
        const InternalKeyComparator* internal_comparator;
        bool operator()(FileMetaData* f1, FileMetaData* f2) const {
            int r = internal_comparator->Compare(f1->smallest, f2->smallest);
            if (r != 0) {
                return (r < 0);
            } else {
                return (f1->number < f2->number);
            }
        }
    };

    typedef std::set<FileMetaData*, BySmallestKey> FileSet;
    struct LevelState {
        std::set<uint64_t> deleted_files;
        FileSet* added_files;
    };

    VersionSet* vset_;
    Version* base_;
    LevelState levels_[kNumLevels];

public:
    Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
        base_->Ref();
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp();
        for (int level = 0; level < kNumLevels; level++) {
            levels_[level].added_files = new FileSet(cmp);
        }
    }

    ~Builder() {
        for (int level = 0; level < kNumLevels; level++) {
            const FileSet* added = levels_[level].added_files;
            std::vector<FileMetaData*> to_unref;
            to_unref.reserve(added->size());
            for (FileSet::const_iterator it = added->begin(); it != added->end(); ++it) {
                to_unref.push_back(*it);
            }
            delete added;
            for (uint32_t i = 0; i < to_unref.size(); i++) {
                FileMetaData* f = to_unref[i];
                f->refs--;
                if (f->refs <= 0) {
                    delete f;
                }
            }
        }
        base_->Unref();
    }

    void Apply(const VersionEdit* edit) {
        for (const auto& del : edit->deleted_files_) {
            const int level = del.first;
            const uint64_t number = del.second;
            levels_[level].deleted_files.insert(number);
        }

        for (size_t i = 0; i < edit->new_files_.size(); i++) {
            const int level = edit->new_files_[i].first;
            FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
            f->refs = 1;
            levels_[level].deleted_files.erase(f->number);
            levels_[level].added_files->insert(f);
        }
    }

    void SaveTo(Version* v) {
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp();
        for (int level = 0; level < kNumLevels; level++) {
            const std::vector<FileMetaData*>& base_files = base_->files_[level];
            std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
            std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
            const FileSet* added = levels_[level].added_files;
            v->files_[level].reserve(base_files.size() + added->size());

            for (const auto& added_file : *added) {
                for (std::vector<FileMetaData*>::const_iterator bpos =
                         std::upper_bound(base_iter, base_end, added_file, cmp);
                     base_iter != bpos; ++base_iter) {
                    MaybeAddFile(v, level, *base_iter);
                }
                MaybeAddFile(v, level, added_file);
            }
            for (; base_iter != base_end; ++base_iter) {
                MaybeAddFile(v, level, *base_iter);
            }
        }
    }

    void MaybeAddFile(Version* v, int level, FileMetaData* f) {
        if (levels_[level].deleted_files.count(f->number) > 0) {
            // File is deleted: do not add
        } else {
            f->refs++;
            v->files_[level].push_back(f);
        }
    }
};

VersionSet::VersionSet(const std::string& dbname, const Options* options,
                       const InternalKeyComparator* cmp)
    : dbname_(dbname),
      options_(options),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(nullptr),
      descriptor_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
    AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
    current_->Unref();
    assert(dummy_versions_.next_ == &dummy_versions_);
    delete descriptor_log_;
    delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
    assert(v->refs_ == 0);
    assert(v != current_);
    if (current_ != nullptr) {
        current_->Unref();
    }
    current_ = v;
    v->Ref();

    v->prev_ = dummy_versions_.prev_;
    v->next_ = &dummy_versions_;
    v->prev_->next_ = v;
    v->next_->prev_ = v;
}

void VersionSet::Finalize(Version* v) {
    // Determine level with worst compaction score
    int best_level = -1;
    double best_score = -1;

    for (int level = 0; level < kNumLevels - 1; level++) {
        double score;
        if (level == 0) {
            // For level 0, score based on number of files
            // (e.g. trigger compaction when L0 >= 4 files)
            score = v->files_[level].size() / 4.0;
        } else {
            // Compute total bytes in level
            const uint64_t level_bytes = TotalFileSize(v->files_[level]);
            score = static_cast<double>(level_bytes) / MaxBytesForLevel(level);
        }

        if (score > best_score) {
            best_score = score;
            best_level = level;
        }
    }

    v->compaction_level_ = best_level;
    v->compaction_score_ = best_score;
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
    if (edit->has_log_number_) {
        assert(edit->log_number_ >= log_number_);
        assert(edit->log_number_ < next_file_number_);
    } else {
        edit->SetLogNumber(log_number_);
    }

    if (!edit->has_prev_log_number_) {
        edit->SetPrevLogNumber(prev_log_number_);
    }

    edit->SetNextFile(next_file_number_);
    edit->SetLastSequence(last_sequence_);

    Version* v = new Version(this);
    {
        Builder builder(this, current_);
        builder.Apply(edit);
        builder.SaveTo(v);
    }
    Finalize(v);

    // Write descriptor/manifest record
    std::string record;
    edit->EncodeTo(&record);

    Status s;
    if (descriptor_log_ == nullptr) {
        // First descriptor write: create MANIFEST file
        assert(descriptor_file_ == nullptr);
        manifest_file_number_ = NewFileNumber();
        std::string manifest_name = DescriptorFileName(dbname_, manifest_file_number_);
        s = Env::NewWritableFile(manifest_name, &descriptor_file_);
        if (s.ok()) {
            descriptor_log_ = new log::Writer(descriptor_file_);
            // Set CURRENT pointing to MANIFEST
            std::string current_name = CurrentFileName(dbname_);
            char manifest_buf[100];
            std::snprintf(manifest_buf, sizeof(manifest_buf), "MANIFEST-%06llu",
                          static_cast<unsigned long long>(manifest_file_number_));
            std::string manifest_base = manifest_buf;
            FILE* cf = fopen((current_name + ".tmp").c_str(), "w");
            if (cf != nullptr) {
                fprintf(cf, "%s\n", manifest_base.c_str());
                fclose(cf);
                rename((current_name + ".tmp").c_str(), current_name.c_str());
            }
        }
    }

    if (s.ok()) {
        s = descriptor_log_->AddRecord(record);
        if (s.ok()) {
            s = descriptor_file_->Sync();
        }
    }

    if (s.ok()) {
        AppendVersion(v);
        log_number_ = edit->log_number_;
        prev_log_number_ = edit->prev_log_number_;
    } else {
        delete v;
    }
    return s;
}

Status VersionSet::Recover() {
    // Read CURRENT file to locate active MANIFEST
    std::string current_name = CurrentFileName(dbname_);
    std::string dscname;
    {
        std::ifstream f(current_name);
        if (!f.is_open()) {
            return Status::NotFound("CURRENT file not found");
        }
        std::getline(f, dscname);
        if (dscname.empty()) {
            return Status::Corruption("CURRENT file is empty");
        }
    }

    std::string manifest_path = dbname_ + "/" + dscname;
    SequentialFile* mfile = nullptr;
    Status s = Env::NewSequentialFile(manifest_path, &mfile);
    if (!s.ok()) return s;

    log::Reader reader(mfile, nullptr, true, 0);
    Slice record;
    std::string scratch;
    Builder builder(this, current_);
    uint64_t next_file = 0;
    uint64_t last_seq = 0;
    uint64_t log_num = 0;
    uint64_t prev_log = 0;

    while (reader.ReadRecord(&record, &scratch)) {
        VersionEdit edit;
        s = edit.DecodeFrom(record);
        if (!s.ok()) {
            delete mfile;
            return s;
        }
        builder.Apply(&edit);
        if (edit.has_next_file_number_) next_file = edit.next_file_number_;
        if (edit.has_last_sequence_) last_seq = edit.last_sequence_;
        if (edit.has_log_number_) log_num = edit.log_number_;
        if (edit.has_prev_log_number_) prev_log = edit.prev_log_number_;
    }
    delete mfile;

    Version* v = new Version(this);
    builder.SaveTo(v);
    Finalize(v);
    AppendVersion(v);

    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_seq;
    log_number_ = log_num;
    prev_log_number_ = prev_log;

    return Status::OK();
}

}  // namespace leveldb
