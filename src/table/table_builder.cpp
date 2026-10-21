#include "table/table_builder.h"
#include <cassert>
#include "db/dbformat.h"
#include "db/log_writer_reader.h"
#include "leveldb/comparator.h"
#include "leveldb/filter_policy.h"
#include "table/block_builder.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
    Options options;
    Options index_block_options;
    WritableFile* file;
    uint64_t offset;
    Status status;
    BlockBuilder data_block;
    BlockBuilder index_block;
    std::string last_key;
    int64_t num_entries;
    bool closed;  // Either Finish() or Abandon() has been called.

    // Filter block state
    std::string filter_data;
    std::vector<std::string> filter_keys;

    // We do not emit the index entry for a block until we have seen the
    // first key for the next block. This allows us to use shorter keys in
    // the index block.
    bool pending_index_entry;
    BlockHandle pending_handle;

    Rep(const Options& opt, WritableFile* f)
        : options(opt),
          index_block_options(opt),
          file(f),
          offset(0),
          data_block(opt.block_restart_interval, opt.comparator),
          index_block(1, opt.comparator),
          num_entries(0),
          closed(false),
          pending_index_entry(false) {}
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {}

TableBuilder::~TableBuilder() {
    assert(rep_->closed);
    delete rep_;
}

uint64_t TableBuilder::NumEntries() const {
    return rep_->num_entries;
}

uint64_t TableBuilder::FileSize() const {
    return rep_->offset;
}

Status TableBuilder::status() const {
    return rep_->status;
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;

    if (r->num_entries > 0) {
        assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
    }

    if (r->pending_index_entry) {
        assert(r->data_block.empty());
        r->options.comparator->FindShortestSeparator(&r->last_key, key);
        std::string handle_encoding;
        r->pending_handle.EncodeTo(&handle_encoding);
        r->index_block.Add(Slice(r->last_key), Slice(handle_encoding));
        r->pending_index_entry = false;
    }

    if (r->options.filter_policy != nullptr) {
        r->filter_keys.push_back(ExtractUserKey(key).ToString());
    }

    r->last_key.assign(key.data(), key.size());
    r->num_entries++;
    r->data_block.Add(key, value);

    const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
    if (estimated_block_size >= r->options.block_size) {
        Flush();
    }
}

void TableBuilder::Flush() {
    Rep* r = rep_;
    assert(!r->closed);
    if (!ok()) return;
    if (r->data_block.empty()) return;
    assert(!r->pending_index_entry);
    WriteBlock(&r->data_block, &r->pending_handle);
    if (ok()) {
        r->pending_index_entry = true;
        r->status = r->file->Flush();
    }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
    // File format contains a sequence of blocks where each block has:
    //    block_data: uint8[n]
    //    type: uint8 (compression type, 0 for none)
    //    crc: uint32
    assert(ok());
    Rep* r = rep_;
    Slice raw = block->Finish();

    WriteRawBlock(raw, handle);
    block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents, BlockHandle* handle) {
    Rep* r = rep_;
    handle->set_offset(r->offset);
    handle->set_size(block_contents.size());
    r->status = r->file->Append(block_contents);
    if (r->status.ok()) {
        char trailer[kBlockTrailerSize];
        trailer[0] = 0;  // kNoCompression
        uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
        crc = crc32c::Extend(crc, trailer, 1);
        EncodeFixed32(trailer + 1, crc32c::Mask(crc));
        r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
        if (r->status.ok()) {
            r->offset += block_contents.size() + kBlockTrailerSize;
        }
    }
}

Status TableBuilder::Finish() {
    Rep* r = rep_;
    Flush();
    assert(!r->closed);
    r->closed = true;

    BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

    // 1. Write filter block if policy is given
    if (ok() && r->options.filter_policy != nullptr) {
        std::vector<Slice> key_slices;
        key_slices.reserve(r->filter_keys.size());
        for (const auto& k : r->filter_keys) {
            key_slices.emplace_back(k);
        }
        std::string filter;
        if (!key_slices.empty()) {
            r->options.filter_policy->CreateFilter(key_slices.data(), static_cast<int>(key_slices.size()), &filter);
        }
        WriteRawBlock(Slice(filter), &filter_block_handle);
    }

    // 2. Write metaindex block
    if (ok()) {
        BlockBuilder metaindex_block(r->options.block_restart_interval);
        if (r->options.filter_policy != nullptr) {
            std::string key = "filter.";
            key.append(r->options.filter_policy->Name());
            std::string handle_encoding;
            filter_block_handle.EncodeTo(&handle_encoding);
            metaindex_block.Add(key, handle_encoding);
        }
        WriteBlock(&metaindex_block, &metaindex_block_handle);
    }

    // 3. Write index block
    if (ok()) {
        if (r->pending_index_entry) {
            r->options.comparator->FindShortSuccessor(&r->last_key);
            std::string handle_encoding;
            r->pending_handle.EncodeTo(&handle_encoding);
            r->index_block.Add(Slice(r->last_key), Slice(handle_encoding));
            r->pending_index_entry = false;
        }
        WriteBlock(&r->index_block, &index_block_handle);
    }

    // 4. Write footer
    if (ok()) {
        Footer footer;
        footer.set_metaindex_handle(metaindex_block_handle);
        footer.set_index_handle(index_block_handle);
        std::string footer_encoding;
        footer.EncodeTo(&footer_encoding);
        r->status = r->file->Append(footer_encoding);
        if (r->status.ok()) {
            r->offset += footer_encoding.size();
        }
    }
    return r->status;
}

void TableBuilder::Abandon() {
    Rep* r = rep_;
    assert(!r->closed);
    r->closed = true;
}

}  // namespace leveldb
