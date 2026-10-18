#include "leveldb/options.h"
#include "leveldb/comparator.h"

namespace leveldb {

Options::Options()
    : comparator(BytewiseComparator()),
      create_if_missing(false),
      error_if_exists(false),
      block_size(4096),
      block_restart_interval(16),
      max_file_size(2 * 1024 * 1024),
      write_buffer_size(4 * 1024 * 1024),
      filter_policy(nullptr) {}

}  // namespace leveldb
