#include <gtest/gtest.h>
#include <raft-kv/server/redis_store.h>
#include <raft-kv/server/raft_node.h>
// #include <time.h>
// #include <unistd.h>
#include <raft-kv/raft/proto.h>
// #include <boost/filesystem.hpp>
#include <msgpack.hpp>  // need install
#include <raft-kv/raft/raft.h>
#include <raft-kv/common/log.h>
#include <raft-kv/raft/util.h>
#include "network.hpp"  // for newTestRaft
#include <unordered_map>
using namespace kv;

// easier way to generate snapshot and initialize raftnode
// noting struct snapshot in proto.h
static proto::Snapshot& testingSnap() {
  static proto::SnapshotPtr snapshot;
  if (!snapshot) {
    snapshot = std::make_shared<proto::Snapshot>();
    snapshot->metadata.index = 11; // 这个index会用于更新raftlog的first index = index + 1
    snapshot->metadata.term = 11;
    snapshot->metadata.conf_state.nodes.push_back(1);
    snapshot->metadata.conf_state.nodes.push_back(2);
    
    std::unordered_map<std::string, std::string> kv;
    kv["key1"] = "value1";
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, kv);
    snapshot->data.assign(sbuf.data(), sbuf.data() + sbuf.size());
  }
  return *snapshot;
}

TEST(RedisStoreTest, InitializeRedis) {
    proto::Snapshot& snapshot = testingSnap(); // noting we just need the serialized kv from 
    // this snapshot for starting redis_store.

    // using snapshot to initialize TestRaftNode and RedisStore
    RaftNode raft_node(1, "/tmp", 8080); // 注意实际上我们会有专门的snap-dir存储快照，所以如果有已有的快照
    // 那么会从这个dir中找到。并且获取最新的一个快照（sort and greater）。但是我们这里为了简化，直接传入了我们创建的snapshot
    RedisStore redis_store(&raft_node, snapshot.data, 8080); // noting the 2nd para is kv (data part of snapshot)

    // check whether raft_node_ key and value is what we put before
    std::string value;
    ASSERT_TRUE(redis_store.get("key1", value));
    EXPECT_EQ(value, "value1");
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}