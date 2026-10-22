#include "db/version_edit.h"
#include "util/coding.h"

namespace leveldb {

// Tag numbers for serialized VersionEdit fields
enum Tag {
    kComparator = 1,
    kLogNumber = 2,
    kNextFileNumber = 3,
    kLastSequence = 4,
    kCompactPointer = 5,
    kDeletedFile = 6,
    kNewFile = 7,
    kPrevLogNumber = 9
};

void VersionEdit::Clear() {
    comparator_.clear();
    log_number_ = 0;
    prev_log_number_ = 0;
    last_sequence_ = 0;
    next_file_number_ = 0;
    has_comparator_ = false;
    has_log_number_ = false;
    has_prev_log_number_ = false;
    has_next_file_number_ = false;
    has_last_sequence_ = false;
    deleted_files_.clear();
    new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
    if (has_comparator_) {
        PutVarint32(dst, kComparator);
        PutLengthPrefixedSlice(dst, comparator_);
    }
    if (has_log_number_) {
        PutVarint32(dst, kLogNumber);
        PutVarint64(dst, log_number_);
    }
    if (has_prev_log_number_) {
        PutVarint32(dst, kPrevLogNumber);
        PutVarint64(dst, prev_log_number_);
    }
    if (has_next_file_number_) {
        PutVarint32(dst, kNextFileNumber);
        PutVarint64(dst, next_file_number_);
    }
    if (has_last_sequence_) {
        PutVarint32(dst, kLastSequence);
        PutVarint64(dst, last_sequence_);
    }

    for (const auto& deleted : deleted_files_) {
        PutVarint32(dst, kDeletedFile);
        PutVarint32(dst, deleted.first);   // level
        PutVarint64(dst, deleted.second);  // file number
    }

    for (const auto& new_file : new_files_) {
        const FileMetaData& f = new_file.second;
        PutVarint32(dst, kNewFile);
        PutVarint32(dst, new_file.first);  // level
        PutVarint64(dst, f.number);
        PutVarint64(dst, f.file_size);
        PutLengthPrefixedSlice(dst, f.smallest.Encode());
        PutLengthPrefixedSlice(dst, f.largest.Encode());
    }
}

static bool GetInternalKey(Slice* input, InternalKey* dst) {
    Slice str;
    if (GetLengthPrefixedSlice(input, &str)) {
        dst->DecodeFrom(str);
        return true;
    } else {
        return false;
    }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
    Clear();
    Slice input = src;
    const char* msg = nullptr;
    uint32_t tag;

    while (msg == nullptr && GetVarint32(&input, &tag)) {
        switch (tag) {
            case kComparator: {
                Slice str;
                if (GetLengthPrefixedSlice(&input, &str)) {
                    comparator_ = str.ToString();
                    has_comparator_ = true;
                } else {
                    msg = "comparator name";
                }
                break;
            }
            case kLogNumber:
                if (GetVarint64(&input, &log_number_)) {
                    has_log_number_ = true;
                } else {
                    msg = "log number";
                }
                break;
            case kPrevLogNumber:
                if (GetVarint64(&input, &prev_log_number_)) {
                    has_prev_log_number_ = true;
                } else {
                    msg = "prev log number";
                }
                break;
            case kNextFileNumber:
                if (GetVarint64(&input, &next_file_number_)) {
                    has_next_file_number_ = true;
                } else {
                    msg = "next file number";
                }
                break;
            case kLastSequence:
                if (GetVarint64(&input, &last_sequence_)) {
                    has_last_sequence_ = true;
                } else {
                    msg = "last sequence number";
                }
                break;
            case kDeletedFile: {
                uint32_t level;
                uint64_t number;
                if (GetVarint32(&input, &level) && GetVarint64(&input, &number)) {
                    deleted_files_.insert(std::make_pair(level, number));
                } else {
                    msg = "deleted file";
                }
                break;
            }
            case kNewFile: {
                uint32_t level;
                FileMetaData f;
                if (GetVarint32(&input, &level) &&
                    GetVarint64(&input, &f.number) &&
                    GetVarint64(&input, &f.file_size) &&
                    GetInternalKey(&input, &f.smallest) &&
                    GetInternalKey(&input, &f.largest)) {
                    new_files_.push_back(std::make_pair(level, f));
                } else {
                    msg = "new-file entry";
                }
                break;
            }
            default:
                msg = "unknown tag";
                break;
        }
    }

    if (msg == nullptr && !input.empty()) {
        msg = "invalid tag";
    }

    Status result;
    if (msg != nullptr) {
        result = Status::Corruption("VersionEdit", msg);
    }
    return result;
}

}  // namespace leveldb
