#include <gtest/gtest.h>
#include <raft-kv/raft/raft.h>
#include <raft-kv/common/log.h>
#include <raft-kv/raft/util.h>
#include "network.hpp"

using namespace kv;
/** 这个代码定义了一系列用于测试Raft协议中快照相关功能的单元测试。leader 根据follower的回应判断是否要
 * 更新快照，重置follower的状态，以及更新到哪一个快照。
 */

/** 这个静态函数创建并返回一个包含特定元数据的快照对象。它确保快照对象在多次调用之间共享。注意这是一个函数
 * 不是一个struct！
 */
static proto::Snapshot& testingSnap() {
  static proto::SnapshotPtr snapshot;
  if (!snapshot) {
    snapshot = std::make_shared<proto::Snapshot>();
    snapshot->metadata.index = 11; // 这个index会用于更新raftlog的first index = index + 1
    snapshot->metadata.term = 11;
    snapshot->metadata.conf_state.nodes.push_back(1);
    snapshot->metadata.conf_state.nodes.push_back(2);

  }
  return *snapshot;
}
/** 测试当一个从节点拒绝追加条目时，主节点是否会为follower设置一个待处理的快照 */
TEST(snap, SendingSnapshotSetPendingSnapshot) {
  auto storage = std::make_shared<MemoryStorage>(); // 创建一个内存存储对象
  auto sm = newTestRaft(1, {1}, 10, 1, storage);  // 使用newTestRaft创建一个Raft状态机sm。
  // 该状态机的ID是1，集群包括节点1，心跳超时是10，选举超时是1.
  sm->restore(testingSnap()); // 恢复快照状态，设置初始的快照为testingSnap的设置（term为11）
  // 注意在快照中有两个节点，1，2.所以从这里开始集群有两个node了。更新了cluster 成员

  sm->become_candidate();
  sm->become_leader();

  // force set the next of node 1, so that
  // node 1 needs a snapshot。强制设置节点2的next索引为raft-log的第一个索引。这意味着节点2需要
  // 一个快照，因为他的next索引已经落后于领导者的日志索引
  sm->prs_[2]->next = sm->raft_log_->first_index(); // 12。当新的日志条目到来时，它们会从 first_index_ 开始。
  // next表示从节点将要从leader接受的下一个日志条目index
  // first index表示已经应用到状态机的最后一个日志条目的下一个索引（如果从快照恢复。否则就是它恢复后的第一个日志索引）
  /** 当从节点的next索引等于主节点的first index时，表示从节点的日志条目已经比主节点的日志起始位置还要落后
   * 在这种情况下，从节点不能简单的通过日志复制来赶上主节点，而是需要接受的一个快照。
   */
  proto::MessagePtr msg(new proto::Message());
  msg->from = 2;
  msg->to = 1;
  msg->type = proto::MsgAppResp;
  msg->index = sm->prs_[2]->next - 1;  // 设置消息的索引落后于当前日志的第一个索引一位
  msg->reject = true;

  sm->step(msg); // 领导者处理从2发出的消息。它处理时会发现node2需要一个快照，因为它的日志条目落后
// 断言节点2 的pending snapshot为11，这是之前快照的索引。这个assert验证了领导者正确的为node2设置了待处理的快照
  ASSERT_EQ(sm->prs_[2]->pending_snapshot, 11);
  /** 主节点正在向从节点发送的快照的最后一个日志条目的索引是 11。 */
}

/** 这个测试是判断leader是否会因为有follower需要接收快照而停止发送新的数据消息，而是处理快照需求 */
TEST(snap, PendingSnapshotPauseReplication) {
  auto storage = std::make_shared<MemoryStorage>();
  auto sm = newTestRaft(1, {1, 2}, 10, 1, storage);
  sm->restore(testingSnap());

  sm->become_candidate();
  sm->become_leader();

  sm->prs_[2]->become_snapshot(11); // 节点2需要一个需要，时间戳为11
// 节点1向自己发送一个数据更新的msg，按理说这个log需要发给他的follower， 来更新data
  proto::MessagePtr msg(new proto::Message());
  msg->from = 1;
  msg->to = 1;
  msg->type = proto::MsgProp;
  proto::Entry entry;
  entry.data = str_to_vector("somedata");
  msg->entries.push_back(entry);

  sm->step(msg);
// 从sm 的message queue读消息到msgs。msgs为空，因为leader发现follower需要更新snapshot，所以他不会
// 将update data 的msg放入queue里
  auto msgs = sm->read_messages();
  ASSERT_TRUE(msgs.empty());
}

/** 这个测试是判断领导者（leader）会根据从节点（follower）是否拒绝快照来决定是否重置pending_snapshot，
 * 而不是仅仅根据从节点的快照索引来判断（become_snapshot(11)）。
 * 这是因为快照传输和应用是一个过程，领导者需要根据从节点的反馈来动态调整同步策略。*/
TEST(snap, SnapshotFailure) {
  auto storage = std::make_shared<MemoryStorage>();
  auto sm = newTestRaft(1, {1, 2}, 10, 1, storage);
  sm->restore(testingSnap());

  sm->become_candidate();
  sm->become_leader();

  sm->prs_[2]->next = 1;  // 设置节点2 的next索引为1
  sm->prs_[2]->become_snapshot(11);  // 节点2需要一个需要，时间戳为11。如果follower接受快照
  // 这个索引会被设置为快照的索引

  proto::MessagePtr msg(new proto::Message());
  msg->from = 2;
  msg->to = 1;
  msg->type = proto::MsgSnapStatus;  // 表示快照状态消息
  msg->reject = true;  // 拒绝快照

  sm->step(msg);
/** 当从节点拒绝接受快照时，领导者会将pending_snapshot重置为0，表示不在等待快照。
 * 重置pending_snapshot的原因是，领导者需要重新评估和决定下一步的行动，
 * 而不是继续等待一个已经被拒绝的快照。领导者可能需要重新发送日志条目或尝试另一个快照。 */
  ASSERT_EQ(sm->prs_[2]->pending_snapshot, 0);
  ASSERT_EQ(sm->prs_[2]->next, 1);
  ASSERT_TRUE(sm->prs_[2]->paused); // 表示从节点仍然需要处理同步问题，可能是重新发送快照或日志条目。
}
/** 这个测试与snapshot failure相对 */
TEST(snap, SnapshotSucceed) {
  auto storage = std::make_shared<MemoryStorage>();
  auto sm = newTestRaft(1, {1, 2}, 10, 1, storage);
  sm->restore(testingSnap());

  sm->become_candidate();
  sm->become_leader();

  sm->prs_[2]->next = 1;  // 虽然这里设置成1.但是leader会根据become-snapshot的idx更新next
  sm->prs_[2]->become_snapshot(11);

  proto::MessagePtr msg(new proto::Message());
  msg->from = 2;
  msg->to = 1;
  msg->type = proto::MsgSnapStatus;
  msg->reject = false;

  sm->step(msg);

  ASSERT_EQ(sm->prs_[2]->pending_snapshot, 0);  // 成功也是不再等待快照。所以这里不是指从0开始更新快照
  ASSERT_EQ(sm->prs_[2]->next, 12);  // next成功更新成12
  ASSERT_TRUE(sm->prs_[2]->paused);  // 尽管成功接收了快照，但日志并不一定同步到最新，仍需要暂停
}

/**  */
TEST(snap, SnapshotAbort) {
  auto storage = std::make_shared<MemoryStorage>();
  auto sm = newTestRaft(1, {1, 2}, 10, 1, storage);
  sm->restore(testingSnap());

  sm->become_candidate();
  sm->become_leader();

  sm->prs_[2]->next = 1;
  sm->prs_[2]->become_snapshot(11);

  // A successful msgAppResp that has a higher/equal index than the
  // pending snapshot should abort the pending snapshot.
  // 如果msgResp比pending的snapshot的idx还要高或者相等。应该放弃目前pending的snapshot。因为
  // 当前节点的状态是最新的，跟需要更新的snapshot一致。所以它应该不需要更新快照，而是可以直接update next
  proto::MessagePtr msg(new proto::Message());
  msg->from = 2;
  msg->to = 1;
  msg->type = proto::MsgAppResp;
  /** 在这个上下文中，msg->index表示从节点2已经成功复制和应用的最新日志条目索引，
   * 即索引为11的日志条目。这意味着从节点2已经包含并应用了所有索引小于等于11的日志条目。 */
  msg->index = 11;

  sm->step(msg);

  ASSERT_EQ(sm->prs_[2]->pending_snapshot, 0);
  ASSERT_EQ(sm->prs_[2]->next, 12);
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS(); // testingSnap（）在这之后run
}
