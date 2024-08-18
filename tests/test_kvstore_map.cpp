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

// /** two method to generate snapshot. 1. generate tmp snapshot dir */
// static std::string get_tmp_snapshot_dir() {
//   char buffer[128];
//   // time(NULL) return current time (second), getpid() return current process id. 
//   // snprintf convert timestamp and pid to string, and combind as a dir
//   // return dir is "_test_snapshot/time_pid", ensure every test generate unique dir
//   snprintf(buffer, sizeof(buffer), "_test_snapshot/%d_%d", (int) time(NULL), getpid());
//   return buffer;
// }

// proto::Snapshot& get_test_snap() {
//   static proto::SnapshotPtr snap;
//   if (!snap) {
//     snap = std::make_shared<proto::Snapshot>();
//     snap->metadata.index = 1;
//     snap->metadata.term = 1;
    
//     snap->data.push_back('t');
//     snap->data.push_back('e');
//     snap->data.push_back('s');
//     snap->data.push_back('t');

//     snap->metadata.conf_state.nodes.push_back(1);
//     snap->metadata.conf_state.nodes.push_back(2);
//     snap->metadata.conf_state.nodes.push_back(3);
//   }
//   return *snap;
// }

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
    proto::Snapshot& snapshot = testingSnap();
    // serialize snapshot data. noting that kv data put in snapshot need serialize
    // then snapshot need another serialize as a whole

    // using snapshot to initialize TestRaftNode and RedisStore
    RaftNode raft_node(1, "/tmp", 8080);
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