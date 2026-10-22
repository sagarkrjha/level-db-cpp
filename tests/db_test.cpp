#include <cassert>
#include <iostream>
#include <string>
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"

int main() {
    std::string dbpath = "/tmp/test_leveldb";
    leveldb::DestroyDB(dbpath, leveldb::Options());

    leveldb::DB* db = nullptr;
    leveldb::Options options;
    options.create_if_missing = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.write_buffer_size = 64 * 1024;  // small buffer to test flushes quickly

    leveldb::Status s = leveldb::DB::Open(options, dbpath, &db);
    assert(s.ok());
    std::cout << "[PASS] DB::Open succeeded." << std::endl;

    // 1. Basic Put and Get
    s = db->Put(leveldb::WriteOptions(), "key1", "value1");
    assert(s.ok());
    std::string val;
    s = db->Get(leveldb::ReadOptions(), "key1", &val);
    assert(s.ok() && val == "value1");
    std::cout << "[PASS] Basic Put and Get: key1 -> " << val << std::endl;

    // 2. Overwrite and Deletion
    s = db->Put(leveldb::WriteOptions(), "key1", "value1_updated");
    assert(s.ok());
    s = db->Get(leveldb::ReadOptions(), "key1", &val);
    assert(s.ok() && val == "value1_updated");

    s = db->Delete(leveldb::WriteOptions(), "key1");
    assert(s.ok());
    s = db->Get(leveldb::ReadOptions(), "key1", &val);
    assert(s.IsNotFound());
    std::cout << "[PASS] Overwrite and Delete verification." << std::endl;

    // 3. WriteBatch atomic operations
    leveldb::WriteBatch batch;
    batch.Put("batch_a", "100");
    batch.Put("batch_b", "200");
    batch.Delete("batch_a");
    batch.Put("batch_c", "300");
    s = db->Write(leveldb::WriteOptions(), &batch);
    assert(s.ok());

    s = db->Get(leveldb::ReadOptions(), "batch_a", &val);
    assert(s.IsNotFound());
    s = db->Get(leveldb::ReadOptions(), "batch_b", &val);
    assert(s.ok() && val == "200");
    s = db->Get(leveldb::ReadOptions(), "batch_c", &val);
    assert(s.ok() && val == "300");
    std::cout << "[PASS] WriteBatch operations." << std::endl;

    // 4. Multiple entries triggering SSTable Minor Compaction & Flush
    std::cout << "Inserting 1000 items to test WAL and SSTable flushing..." << std::endl;
    for (int i = 0; i < 1000; i++) {
        char k[32], v[64];
        std::snprintf(k, sizeof(k), "user:%05d", i);
        std::snprintf(v, sizeof(v), "data_payload_%05d_leveldb_test", i);
        s = db->Put(leveldb::WriteOptions(), k, v);
        assert(s.ok());
    }

    // Force compaction
    db->CompactRange(nullptr, nullptr);

    // Verify all 1000 items after compaction
    for (int i = 0; i < 1000; i++) {
        char k[32], expected[64];
        std::snprintf(k, sizeof(k), "user:%05d", i);
        std::snprintf(expected, sizeof(expected), "data_payload_%05d_leveldb_test", i);
        s = db->Get(leveldb::ReadOptions(), k, &val);
        if (!s.ok()) {
            std::cerr << "FAILED at key: " << k << " status: " << s.ToString() << std::endl;
        }
        assert(s.ok() && val == expected);
    }
    std::cout << "[PASS] 1000 items verified across SSTables." << std::endl;

    // 5. Iterator Range Scan
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek("user:00100");
    int count = 0;
    while (it->Valid() && it->key().starts_with("user:")) {
        count++;
        it->Next();
    }
    delete it;
    assert(count == 900);
    std::cout << "[PASS] Iterator range scan matched count: " << count << std::endl;

    // 6. Persistence & Crash Recovery Test: close and reopen DB
    delete db;
    db = nullptr;

    options.create_if_missing = false;
    s = leveldb::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
        std::cerr << "Open failed: " << s.ToString() << std::endl;
    }
    assert(s.ok());
    s = db->Get(leveldb::ReadOptions(), "user:00555", &val);
    assert(s.ok() && val == "data_payload_00555_leveldb_test");
    std::cout << "[PASS] Reopening DB & persistence verified successfully." << std::endl;

    delete db;
    delete options.filter_policy;
    leveldb::DestroyDB(dbpath, options);
    std::cout << "All LevelDB tests PASSED!" << std::endl;
    return 0;
}
