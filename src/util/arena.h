#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

namespace leveldb {

class Arena {
public:
    Arena() : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}
    ~Arena() {
        for (size_t i = 0; i < blocks_.size(); i++) {
            delete[] blocks_[i];
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    char* Allocate(size_t bytes) {
        assert(bytes > 0);
        if (bytes <= alloc_bytes_remaining_) {
            char* result = alloc_ptr_;
            alloc_ptr_ += bytes;
            alloc_bytes_remaining_ -= bytes;
            return result;
        }
        return AllocateFallback(bytes);
    }

    char* AllocateAligned(size_t bytes) {
        const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
        static_assert((align & (align - 1)) == 0, "Pointer size should be a power of 2");
        size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
        size_t slop = (current_mod == 0 ? 0 : align - current_mod);
        size_t needed = bytes + slop;
        char* result;
        if (needed <= alloc_bytes_remaining_) {
            result = alloc_ptr_ + slop;
            alloc_ptr_ += needed;
            alloc_bytes_remaining_ -= needed;
        } else {
            result = AllocateFallback(bytes);
        }
        assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
        return result;
    }

    size_t MemoryUsage() const {
        return memory_usage_.load(std::memory_order_relaxed);
    }

private:
    char* AllocateFallback(size_t bytes) {
        if (bytes > kBlockSize / 4) {
            // Allocate separate block directly
            char* result = AllocateNewBlock(bytes);
            return result;
        }
        alloc_ptr_ = AllocateNewBlock(kBlockSize);
        alloc_bytes_remaining_ = kBlockSize;

        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        return result;
    }

    char* AllocateNewBlock(size_t block_bytes) {
        char* result = new char[block_bytes];
        blocks_.push_back(result);
        memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
        return result;
    }

    static const int kBlockSize = 4096;
    char* alloc_ptr_;
    size_t alloc_bytes_remaining_;
    std::vector<char*> blocks_;
    std::atomic<size_t> memory_usage_;
};

}  // namespace leveldb
