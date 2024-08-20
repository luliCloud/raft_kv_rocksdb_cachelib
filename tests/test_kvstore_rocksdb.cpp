#include <gtest/gtest.h>
#include <raft-kv/server/redis_store.h>
#include <raft-kv/server/raft_node.h>
#include <raft-kv/raft/proto.h>

#include <raft-kv/raft/raft.h>
#include <raft-kv/common/log.h>
#include <raft-kv/raft/util.h>
#include "network.hpp"  // for newTestRaft
#include <rocksdb/db.h>
#include <rocksdb/utilities/checkpoint.h> 
#include <rocksdb/options.h> // for rocksdb
#include <rocksdb/iterator.h>  // for rocksdb
#include <rocksdb/write_batch.h> // for rocksdb

#include <boost/filesystem.hpp>
using namespace kv;
namespace fs = boost::filesystem;

/** delete the existing folder as we need to start new test */
void deleteDirIfExist(uint64_t id) {
  std::string work_dir = "node_" + std::to_string(id);
  fs::path folderPath(work_dir);

  if (fs::exists(folderPath) && fs::is_directory(folderPath)) {
    try {
      fs::remove_all(folderPath);
      LOG_INFO("Folder deleted successfully.");
    } catch (const fs::filesystem_error& e) {
      LOG_ERROR("Error deleting folder: %s", e.what());
    }
  } else {
    LOG_INFO("Folder does not exist.");
  }
}

rocksdb::DB* generateDB(uint64_t id) {
    std::string work_dir = "node_" + std::to_string(id);
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB* db;
    rocksdb::Status status = rocksdb::DB::Open(options, work_dir, &db);
    if (!status.ok()) {
        LOG_ERROR("Failed to open RocksDB: %s", status.ToString()); 
    }

    std::string key = "key1";
    std::string value = "value1";

    status = db->Put(rocksdb::WriteOptions(), key, value);
    if (!status.ok()) {
      LOG_ERROR("Failed to write to RocksDB: %s", status.ToString());
      delete db;
    }
    return db;
}


TEST(RedisStoreRocksTest, ReadWriteAllKV) {
  deleteDirIfExist(1);
  
  std::vector<uint8_t> empty_snap;
  /** initialize RaftNode and RedisStore */
  RaftNode raft_node(1, "/tmp", 8080);
  RedisStore redis_store(&raft_node, empty_snap, 8080, 1);
  // put one kv into db
  std::string key = "key1";
  std::string value = "value1";

  redis_store.putKV(rocksdb::WriteOptions(), key, value);
  std::string key2 = "key2";
  std::string value2 = "value2";
  redis_store.putKV(rocksdb::WriteOptions(), key2, value2);

  std::string v, k = "key1";
  redis_store.getKV(rocksdb::ReadOptions(), k, v);
  EXPECT_EQ(v, "value1");
  
  // get all key values
  std::vector<std::pair<std::string, std::string>> key_values;
  redis_store.get_all_key_value(key_values);

  // change key2-valu2, and insert k3-v3 in orignal db
  std::string key3 = "key3";
  std::string value3 = "value3";
  redis_store.putKV(rocksdb::WriteOptions(), key3, value3);
  std::string v3, k3 = "key3";
  redis_store.getKV(rocksdb::ReadOptions(), k3, v3);
  EXPECT_EQ(v3, "value3");

  std::string newValue2 = "newValue2";
  redis_store.putKV(rocksdb::WriteOptions(), key2, newValue2);
  std::string v2, k2 = "key2";
  redis_store.getKV(rocksdb::ReadOptions(), k2, v2);
  EXPECT_EQ(v2, "newValue2");
  // now the rocksdb should be {{"key1", "value1"}, {"key2", "newValue2"}, {"key3, value3"}}


  // restore to new key values
  redis_store.load_kv_to_rocksdb(key_values);
  // now the rocksdb should be {{"key1", "value1"}, {"key2", "value2"}}

  std::string returnK3 = "key3", returnV3;
  rocksdb::Status status = redis_store.getKV(rocksdb::ReadOptions(), returnK3, returnV3);
  ASSERT_TRUE(status.IsNotFound());  // should not have key3

  std::string returnK2 = "key2", returnV2;
  status = redis_store.getKV(rocksdb::ReadOptions(), returnK2, returnV2);
  ASSERT_FALSE(status.IsNotFound());  // should have key3
  EXPECT_EQ(returnV2, "value2");
}

// /** test set */

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}