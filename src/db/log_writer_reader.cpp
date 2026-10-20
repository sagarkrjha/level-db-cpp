#include "db/log_writer_reader.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCRC(uint32_t* type_crc) {
    for (int i = 0; i <= kMaxRecordType; i++) {
        char c = static_cast<char>(i);
        type_crc[i] = crc32c::Value(&c, 1);
    }
}

Writer::Writer(WritableFile* dest)
    : dest_(dest), block_offset_(0) {
    InitTypeCRC(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
    InitTypeCRC(type_crc_);
}

Status Writer::AddRecord(const Slice& slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();

    Status s;
    bool begin = true;
    do {
        const int leftover = kBlockSize - block_offset_;
        assert(leftover >= 0);
        if (leftover < kHeaderSize) {
            // Switch to a new block
            if (leftover > 0) {
                // Fill with zero bytes
                static const char zeroes[kHeaderSize] = {0};
                s = dest_->Append(Slice(zeroes, leftover));
                if (!s.ok()) return s;
            }
            block_offset_ = 0;
        }

        // Invariant: we never leave < kHeaderSize bytes in a block
        assert(kBlockSize - block_offset_ >= kHeaderSize);

        const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
        const size_t fragment_length = (left < avail) ? left : avail;

        RecordType type;
        const bool end = (left == fragment_length);
        if (begin && end) {
            type = kFullType;
        } else if (begin) {
            type = kFirstType;
        } else if (end) {
            type = kLastType;
        } else {
            type = kMiddleType;
        }

        s = EmitPhysicalRecord(type, ptr, fragment_length);
        ptr += fragment_length;
        left -= fragment_length;
        begin = false;
    } while (s.ok() && left > 0);
    return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
    assert(length <= 0xffff);  // Must fit in 16 bits
    assert(block_offset_ + kHeaderSize + length <= static_cast<size_t>(kBlockSize));

    // Format header
    char buf[kHeaderSize];
    buf[4] = static_cast<char>(length & 0xff);
    buf[5] = static_cast<char>(length >> 8);
    buf[6] = static_cast<char>(t);

    // Compute checksum
    uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
    crc = crc32c::Mask(crc);
    EncodeFixed32(buf, crc);

    // Write header and payload
    Status s = dest_->Append(Slice(buf, kHeaderSize));
    if (s.ok()) {
        s = dest_->Append(Slice(ptr, length));
        if (s.ok()) {
            s = dest_->Flush();
        }
    }
    block_offset_ += kHeaderSize + length;
    return s;
}

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

bool Reader::SkipToInitialBlock() {
    const size_t offset_in_block = initial_offset_ % kBlockSize;
    uint64_t block_start_location = initial_offset_ - offset_in_block;

    // Don't search a block if we'd be in the trailer
    if (offset_in_block > kBlockSize - 6) {
        block_start_location += kBlockSize;
    }

    end_of_buffer_offset_ = block_start_location;

    // Skip to start of first block that can contain the initial record
    if (block_start_location > 0) {
        Status skip_status = file_->Skip(block_start_location);
        if (!skip_status.ok()) {
            ReportDrop(block_start_location, skip_status);
            return false;
        }
    }
    return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
    if (last_record_offset_ < initial_offset_) {
        if (!SkipToInitialBlock()) {
            return false;
        }
    }

    scratch->clear();
    record->clear();
    bool in_fragmented_record = false;
    uint64_t prospective_record_offset = 0;

    Slice fragment;
    while (true) {
        uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size();
        const unsigned int record_type = ReadPhysicalRecord(&fragment);

        if (resyncing_) {
            if (record_type == kMiddleType) {
                continue;
            } else if (record_type == kLastType) {
                resyncing_ = false;
                continue;
            } else {
                resyncing_ = false;
            }
        }

        switch (record_type) {
            case kFullType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "partial record without end(1)");
                }
                prospective_record_offset = physical_record_offset;
                scratch->clear();
                *record = fragment;
                last_record_offset_ = prospective_record_offset;
                return true;

            case kFirstType:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "partial record without end(2)");
                }
                prospective_record_offset = physical_record_offset;
                scratch->assign(fragment.data(), fragment.size());
                in_fragmented_record = true;
                break;

            case kMiddleType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "missing start of fragmented record(1)");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                }
                break;

            case kLastType:
                if (!in_fragmented_record) {
                    ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
                } else {
                    scratch->append(fragment.data(), fragment.size());
                    *record = Slice(*scratch);
                    last_record_offset_ = prospective_record_offset;
                    return true;
                }
                break;

            case kEof:
                if (in_fragmented_record) {
                    scratch->clear();
                }
                return false;

            case kBadRecord:
                if (in_fragmented_record) {
                    ReportCorruption(scratch->size(), "error in middle of record");
                    in_fragmented_record = false;
                    scratch->clear();
                }
                break;

            default: {
                char buf[40];
                snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
                ReportCorruption(
                    (fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
                    buf);
                in_fragmented_record = false;
                scratch->clear();
                break;
            }
        }
    }
    return false;
}

unsigned int Reader::ReadPhysicalRecord(Slice* result) {
    while (true) {
        if (buffer_.size() < kHeaderSize) {
            if (!eof_) {
                buffer_.clear();
                Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
                end_of_buffer_offset_ += buffer_.size();
                if (!status.ok()) {
                    buffer_.clear();
                    ReportDrop(kBlockSize, status);
                    eof_ = true;
                    return kEof;
                } else if (buffer_.size() < kBlockSize) {
                    eof_ = true;
                }
                continue;
            } else {
                buffer_.clear();
                return kEof;
            }
        }

        // Parse header
        const char* header = buffer_.data();
        const uint32_t a = static_cast<uint8_t>(header[4]);
        const uint32_t b = static_cast<uint8_t>(header[5]);
        const unsigned int type = header[6];
        const uint32_t length = a | (b << 8);

        if (kHeaderSize + length > buffer_.size()) {
            size_t drop_size = buffer_.size();
            buffer_.clear();
            if (!eof_) {
                ReportCorruption(drop_size, "bad record length");
                return kBadRecord;
            }
            return kEof;
        }

        if (type == kZeroType && length == 0) {
            // Skip zero padding
            buffer_.clear();
            return kBadRecord;
        }

        // Checksum verification
        if (checksum_) {
            uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
            uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
            if (actual_crc != expected_crc) {
                size_t drop_size = buffer_.size();
                buffer_.clear();
                ReportCorruption(drop_size, "checksum mismatch");
                return kBadRecord;
            }
        }

        buffer_.remove_prefix(kHeaderSize + length);
        if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_) {
            result->clear();
            return kBadRecord;
        }

        *result = Slice(header + kHeaderSize, length);
        return type;
    }
}

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
    ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
    if (reporter_ != nullptr &&
        end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
        reporter_->Corruption(static_cast<size_t>(bytes), reason);
    }
}

}  // namespace log
}  // namespace leveldb
