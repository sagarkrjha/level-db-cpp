#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/write_batch.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace test {

// 1. Primitive Coding Tests
TEST(CodingTest, Fixed32And64) {
    char buf[8];
    EncodeFixed32(buf, 0x12345678);
    EXPECT_EQ(DecodeFixed32(buf), 0x12345678);

    EncodeFixed64(buf, 0x0102030405060708ull);
    EXPECT_EQ(DecodeFixed64(buf), 0x0102030405060708ull);
}

TEST(CodingTest, Varint32And64) {
    std::string s;
    PutVarint32(&s, 0);
    PutVarint32(&s, 127);
    PutVarint32(&s, 128);
    PutVarint32(&s, 16383);
    PutVarint32(&s, ~0u);

    Slice input(s);
    uint32_t v;
    EXPECT_TRUE(GetVarint32(&input, &v));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(GetVarint32(&input, &v));
    EXPECT_EQ(v, 127u);
    EXPECT_TRUE(GetVarint32(&input, &v));
    EXPECT_EQ(v, 128u);
    EXPECT_TRUE(GetVarint32(&input, &v));
    EXPECT_EQ(v, 16383u);
    EXPECT_TRUE(GetVarint32(&input, &v));
    EXPECT_EQ(v, ~0u);
    EXPECT_TRUE(input.empty());
}

TEST(Crc32cTest, StandardValues) {
    std::string data = "123456789";
    uint32_t crc = crc32c::Value(data.data(), data.size());
    EXPECT_EQ(crc, 0xe3069283);

    uint32_t masked = crc32c::Mask(crc);
    EXPECT_NE(masked, crc);
    EXPECT_EQ(crc32c::Unmask(masked), crc);
}

// 2. BloomFilter Tests
TEST(FilterTest, BasicBloomFilter) {
    std::unique_ptr<const FilterPolicy> policy(NewBloomFilterPolicy(10));
    std::vector<std::string> keys = {"hello", "world", "leveldb", "google"};
    std::vector<Slice> slices;
    for (const auto& k : keys) slices.emplace_back(k);

    std::string filter;
    policy->CreateFilter(slices.data(), slices.size(), &filter);

    // True positives
    for (const auto& k : keys) {
        EXPECT_TRUE(policy->KeyMayMatch(k, filter));
    }

    // Likely true negatives
    EXPECT_FALSE(policy->KeyMayMatch("missing_key_1", filter));
    EXPECT_FALSE(policy->KeyMayMatch("missing_key_2", filter));
}

// 3. Database Fixture Tests
class DBTest : public ::testing::Test {
protected:
    std::string dbpath_;
    Options options_;
    DB* db_;

    void SetUp() override {
        dbpath_ = "/tmp/test_leveldb_gtest";
        DestroyDB(dbpath_, Options());
        options_.create_if_missing = true;
        options_.filter_policy = NewBloomFilterPolicy(10);
        options_.write_buffer_size = 64 * 1024;
        Status s = DB::Open(options_, dbpath_, &db_);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }

    void TearDown() override {
        delete db_;
        delete options_.filter_policy;
        DestroyDB(dbpath_, Options());
    }

    void Reopen() {
        delete db_;
        db_ = nullptr;
        options_.create_if_missing = false;
        Status s = DB::Open(options_, dbpath_, &db_);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }
};

TEST_F(DBTest, EmptyDB) {
    std::string val;
    Status s = db_->Get(ReadOptions(), "nonexistent", &val);
    EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, PutAndGet) {
    Status s = db_->Put(WriteOptions(), "k1", "v1");
    EXPECT_TRUE(s.ok());

    std::string val;
    s = db_->Get(ReadOptions(), "k1", &val);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(val, "v1");

    // Overwrite
    s = db_->Put(WriteOptions(), "k1", "v2");
    EXPECT_TRUE(s.ok());
    s = db_->Get(ReadOptions(), "k1", &val);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(val, "v2");
}

TEST_F(DBTest, Delete) {
    db_->Put(WriteOptions(), "k1", "v1");
    Status s = db_->Delete(WriteOptions(), "k1");
    EXPECT_TRUE(s.ok());

    std::string val;
    s = db_->Get(ReadOptions(), "k1", &val);
    EXPECT_TRUE(s.IsNotFound());
}

TEST_F(DBTest, AtomicWriteBatch) {
    WriteBatch batch;
    batch.Put("batch_1", "100");
    batch.Put("batch_2", "200");
    batch.Delete("batch_1");
    batch.Put("batch_3", "300");

    Status s = db_->Write(WriteOptions(), &batch);
    EXPECT_TRUE(s.ok());

    std::string val;
    EXPECT_TRUE(db_->Get(ReadOptions(), "batch_1", &val).IsNotFound());
    EXPECT_TRUE(db_->Get(ReadOptions(), "batch_2", &val).ok());
    EXPECT_EQ(val, "200");
    EXPECT_TRUE(db_->Get(ReadOptions(), "batch_3", &val).ok());
    EXPECT_EQ(val, "300");
}

TEST_F(DBTest, CompactionAndPersistence) {
    // Write 1000 keys to trigger memtable flushes & minor compactions
    for (int i = 0; i < 1000; i++) {
        char k[32], v[64];
        std::snprintf(k, sizeof(k), "key:%05d", i);
        std::snprintf(v, sizeof(v), "value_payload_%05d", i);
        Status s = db_->Put(WriteOptions(), k, v);
        ASSERT_TRUE(s.ok());
    }

    // Force compaction
    db_->CompactRange(nullptr, nullptr);

    // Verify all keys
    for (int i = 0; i < 1000; i++) {
        char k[32], expected[64];
        std::snprintf(k, sizeof(k), "key:%05d", i);
        std::snprintf(expected, sizeof(expected), "value_payload_%05d", i);
        std::string val;
        Status s = db_->Get(ReadOptions(), k, &val);
        ASSERT_TRUE(s.ok()) << "Failed to find key " << k;
        EXPECT_EQ(val, expected);
    }

    // Test Iterator range scan
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->Seek("key:00200");
    int count = 0;
    while (it->Valid() && it->key().starts_with("key:")) {
        count++;
        it->Next();
    }
    EXPECT_EQ(count, 800);

    // Reopen DB to verify persistence & crash-recovery
    Reopen();

    std::string val;
    Status s = db_->Get(ReadOptions(), "key:00777", &val);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(val, "value_payload_00777");
}

}  // namespace test
}  // namespace leveldb
