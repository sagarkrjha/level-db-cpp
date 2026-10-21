#include "table/table.h"
#include "db/dbformat.h"
#include "db/log_writer_reader.h"
#include "leveldb/comparator.h"
#include "leveldb/filter_policy.h"
#include "table/block.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct Table::Rep {
    Options options;
    Status status;
    RandomAccessFile* file;
    BlockHandle metaindex_handle;
    Block* index_block;
    std::string filter_data;

    Rep() : file(nullptr), index_block(nullptr) {}
    ~Rep() {
        delete index_block;
    }
};

static Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                        const BlockHandle& handle, BlockContents* result) {
    result->data = Slice();
    result->cachable = false;
    result->heap_allocated = false;

    // Read the block contents as well as the type/crc trailer:
    size_t n = static_cast<size_t>(handle.size());
    char* buf = new char[n + kBlockTrailerSize];
    Slice contents;
    Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
    if (!s.ok()) {
        delete[] buf;
        return s;
    }
    if (contents.size() != n + kBlockTrailerSize) {
        delete[] buf;
        return Status::Corruption("truncated block read");
    }

    // Check the crc of the type and the block contents
    const char* data = contents.data();
    if (options.verify_checksums) {
        const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
        const uint32_t actual = crc32c::Value(data, n + 1);
        if (actual != crc) {
            delete[] buf;
            return Status::Corruption("block checksum mismatch");
        }
    }

    result->data = Slice(data, n);
    result->heap_allocated = true;
    result->cachable = true;
    return Status::OK();
}

Status Table::Open(const Options& options, RandomAccessFile* file,
                   uint64_t size, Table** table) {
    *table = nullptr;
    if (size < Footer::kEncodedLength) {
        return Status::Corruption("file is too short to be an sstable");
    }

    char footer_space[Footer::kEncodedLength];
    Slice footer_input;
    Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                          &footer_input, footer_space);
    if (!s.ok()) return s;

    Footer footer;
    s = footer.DecodeFrom(&footer_input);
    if (!s.ok()) return s;

    // Read index block
    BlockContents index_block_contents;
    ReadOptions opt;
    s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);
    if (!s.ok()) return s;

    Block* index_block = new Block(index_block_contents);
    Rep* rep = new Rep();
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;

    // Read filter block if present
    if (options.filter_policy != nullptr && footer.metaindex_handle().size() > 0) {
        BlockContents metaindex_contents;
        if (ReadBlock(file, opt, footer.metaindex_handle(), &metaindex_contents).ok()) {
            Block metaindex_block(metaindex_contents);
            std::unique_ptr<Iterator> meta_iter(metaindex_block.NewIterator(BytewiseComparator()));
            std::string filter_key = "filter.";
            filter_key.append(options.filter_policy->Name());
            meta_iter->Seek(filter_key);
            if (meta_iter->Valid() && meta_iter->key() == filter_key) {
                Slice filter_handle_slice = meta_iter->value();
                BlockHandle filter_handle;
                if (filter_handle.DecodeFrom(&filter_handle_slice).ok()) {
                    char* filter_buf = new char[filter_handle.size() + kBlockTrailerSize];
                    Slice filter_contents;
                    if (file->Read(filter_handle.offset(), filter_handle.size() + kBlockTrailerSize,
                                   &filter_contents, filter_buf).ok()) {
                        rep->filter_data.assign(filter_contents.data(), filter_handle.size());
                    }
                    delete[] filter_buf;
                }
            }
        }
    }

    *table = new Table(rep);
    return Status::OK();
}

Table::~Table() {
    delete rep_;
}

Iterator* Table::BlockReader(void* arg, const ReadOptions& options, const Slice& index_value) {
    Table* table = reinterpret_cast<Table*>(arg);
    BlockHandle handle;
    Slice input = index_value;
    Status s = handle.DecodeFrom(&input);
    if (!s.ok()) {
        return NewErrorIterator(s);
    }

    BlockContents contents;
    s = ReadBlock(table->rep_->file, options, handle, &contents);
    if (!s.ok()) {
        return NewErrorIterator(s);
    }
    Block* block = new Block(contents);
    return block->NewIterator(table->rep_->options.comparator);
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
    return NewTwoLevelIterator(
        rep_->index_block->NewIterator(rep_->options.comparator),
        &Table::BlockReader, const_cast<Table*>(this), options);
}

bool Table::InternalGet(const ReadOptions& options, const Slice& k,
                        void* arg,
                        void (*handle_result)(void* arg, const Slice& k, const Slice& v)) {
    Status s;
    if (rep_->options.filter_policy != nullptr && !rep_->filter_data.empty()) {
        if (!rep_->options.filter_policy->KeyMayMatch(ExtractUserKey(k), rep_->filter_data)) {
            // Not in filter!
            return false;
        }
    }

    Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
    iiter->Seek(k);
    if (iiter->Valid()) {
        Slice handle_value = iiter->value();
        Iterator* diter = BlockReader(this, options, handle_value);
        diter->Seek(k);
        if (diter->Valid()) {
            ParsedInternalKey parsed_key;
            if (ParseInternalKey(diter->key(), &parsed_key)) {
                // Check if user keys match
                Slice user_k = ExtractUserKey(k);
                if (user_k.compare(parsed_key.user_key) == 0) {
                    (*handle_result)(arg, diter->key(), diter->value());
                }
            }
        }
        s = diter->status();
        delete diter;
    }
    if (s.ok()) {
        s = iiter->status();
    }
    delete iiter;
    return true;
}

}  // namespace leveldb
