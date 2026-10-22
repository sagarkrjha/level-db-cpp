#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace leveldb {

enum FileType {
    kLogFile,
    kDBLockFile,
    kTableFile,
    kDescriptorFile,
    kCurrentFile,
    kTempFile,
    kInfoLogFile
};

// Return the name of the log file with the specified number
inline std::string LogFileName(const std::string& dbname, uint64_t number) {
    char buf[100];
    std::snprintf(buf, sizeof(buf), "/%06llu.log", static_cast<unsigned long long>(number));
    return dbname + buf;
}

// Return the name of the sstable with the specified number
inline std::string TableFileName(const std::string& dbname, uint64_t number) {
    char buf[100];
    std::snprintf(buf, sizeof(buf), "/%06llu.ldb", static_cast<unsigned long long>(number));
    return dbname + buf;
}

// Return the name of the descriptor (MANIFEST) file
inline std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
    char buf[100];
    std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
    return dbname + buf;
}

// Return the name of the CURRENT file
inline std::string CurrentFileName(const std::string& dbname) {
    return dbname + "/CURRENT";
}

// Return the name of the lock file
inline std::string LockFileName(const std::string& dbname) {
    return dbname + "/LOCK";
}

}  // namespace leveldb
