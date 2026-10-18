#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include "db/log_writer_reader.h"
#include "leveldb/status.h"

namespace leveldb {

class PosixSequentialFile : public SequentialFile {
private:
    std::string filename_;
    FILE* file_;

public:
    PosixSequentialFile(std::string fname, FILE* f)
        : filename_(std::move(fname)), file_(f) {}
    ~PosixSequentialFile() override { fclose(file_); }

    Status Read(size_t n, Slice* result, char* scratch) override {
        Status s;
        size_t r = fread_unlocked(scratch, 1, n, file_);
        *result = Slice(scratch, r);
        if (r < n) {
            if (feof(file_)) {
                // EOF is not an error
            } else {
                s = Status::IOError(filename_, strerror(errno));
            }
        }
        return s;
    }

    Status Skip(uint64_t n) override {
        if (fseek(file_, n, SEEK_CUR)) {
            return Status::IOError(filename_, strerror(errno));
        }
        return Status::OK();
    }
};

class PosixRandomAccessFile : public RandomAccessFile {
private:
    std::string filename_;
    int fd_;

public:
    PosixRandomAccessFile(std::string fname, int fd)
        : filename_(std::move(fname)), fd_(fd) {}
    ~PosixRandomAccessFile() override { close(fd_); }

    Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
        Status s;
        ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
        *result = Slice(scratch, (r < 0) ? 0 : r);
        if (r < 0) {
            s = Status::IOError(filename_, strerror(errno));
        }
        return s;
    }
};

class PosixWritableFile : public WritableFile {
private:
    std::string filename_;
    FILE* file_;

public:
    PosixWritableFile(std::string fname, FILE* f)
        : filename_(std::move(fname)), file_(f) {}
    ~PosixWritableFile() override {
        if (file_ != nullptr) {
            fclose(file_);
        }
    }

    Status Append(const Slice& data) override {
        size_t r = fwrite_unlocked(data.data(), 1, data.size(), file_);
        if (r != data.size()) {
            return Status::IOError(filename_, strerror(errno));
        }
        return Status::OK();
    }

    Status Close() override {
        Status result;
        if (fclose(file_) != 0) {
            result = Status::IOError(filename_, strerror(errno));
        }
        file_ = nullptr;
        return result;
    }

    Status Flush() override {
        if (fflush_unlocked(file_) != 0) {
            return Status::IOError(filename_, strerror(errno));
        }
        return Status::OK();
    }

    Status Sync() override {
        if (fflush_unlocked(file_) != 0 ||
            fdatasync(fileno(file_)) != 0) {
            return Status::IOError(filename_, strerror(errno));
        }
        return Status::OK();
    }
};

class Env {
public:
    static Status NewSequentialFile(const std::string& fname, SequentialFile** result) {
        FILE* f = fopen(fname.c_str(), "re");
        if (f == nullptr) {
            *result = nullptr;
            return Status::IOError(fname, strerror(errno));
        } else {
            *result = new PosixSequentialFile(fname, f);
            return Status::OK();
        }
    }

    static Status NewRandomAccessFile(const std::string& fname, RandomAccessFile** result) {
        int fd = open(fname.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            *result = nullptr;
            return Status::IOError(fname, strerror(errno));
        }
        *result = new PosixRandomAccessFile(fname, fd);
        return Status::OK();
    }

    static Status NewWritableFile(const std::string& fname, WritableFile** result) {
        FILE* f = fopen(fname.c_str(), "we");
        if (f == nullptr) {
            *result = nullptr;
            return Status::IOError(fname, strerror(errno));
        } else {
            *result = new PosixWritableFile(fname, f);
            return Status::OK();
        }
    }

    static Status NewAppendableFile(const std::string& fname, WritableFile** result) {
        FILE* f = fopen(fname.c_str(), "ae");
        if (f == nullptr) {
            *result = nullptr;
            return Status::IOError(fname, strerror(errno));
        } else {
            *result = new PosixWritableFile(fname, f);
            return Status::OK();
        }
    }

    static bool FileExists(const std::string& fname) {
        return access(fname.c_str(), F_OK) == 0;
    }

    static Status CreateDir(const std::string& name) {
        if (mkdir(name.c_str(), 0755) != 0) {
            if (errno == EEXIST) {
                return Status::OK();
            }
            return Status::IOError(name, strerror(errno));
        }
        return Status::OK();
    }

    static Status DeleteFile(const std::string& fname) {
        if (unlink(fname.c_str()) != 0) {
            return Status::IOError(fname, strerror(errno));
        }
        return Status::OK();
    }
};

}  // namespace leveldb
