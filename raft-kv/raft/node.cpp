#include <raft-kv/raft/node.h>
#include <raft-kv/common/log.h>
// raft_ is raftptr, 是raft的实例。每个RaftNode对应一个管理它的raft，来进行raft协议相关的活动。
/**
 * RawNode类的作用
RawNode类通常是Raft类的一个包装器，用于简化上层应用与Raft协议的交互。
它提供了一些高级接口，如propose、step、advance等，
以便上层应用可以方便地提出新的日志条目、处理接收到的消息以及推进节点状态。
 */
namespace kv {
  /** 注意在cpp中并没有写class Node{};这样的结构。别的cpp文件中如果写了class Node{} 
   * 那就是在header file中没有声明这个class。否则在cpp中应该写成Node::funcA()这样才对
   */
/** see line 18 for RawNode constructor */
Node* Node::start_node(const Config& conf, const std::vector<PeerContext>& peers) {
  return new RawNode(conf, peers);
}

Node* Node::restart_node(const Config& conf) {
  return new RawNode(conf);
}

/** RawNode's functions starting here.  */ 
RawNode::RawNode(const Config& conf, const std::vector<PeerContext>& peers) {
  // 创建Raft 对象，通过conf初始化一个Raft对象。assign 给raft_ member
  raft_ = std::make_shared<Raft>(conf); // Raft用于实现共识算法，而不是代表一个RaftNode

  uint64_t last_index = 0; // 存储最后一个日志条目的index
  // 通过存储对象检查最后一个日志条目的索引。以确定床架新的RawNode还是恢复现有的RawNode
  Status status = conf.storage->last_index(last_index);
  if (!status.is_ok()) {
    LOG_FATAL("%s", status.to_string().c_str());
  }

  // If the log is empty, this is a new RawNode (like StartNode); otherwise it's
  // restoring an existing RawNode (like RestartNode).
  if (last_index == 0) { // 如果日志是空的，则这是一个新的RawNode
    raft_->become_follower(1, 0); // 将节点设置为初始的跟随者状态，任期为1，领导ID为0

    std::vector<proto::EntryPtr> entries; // 用于存储初始的配置变更条目
// 注意是所有的节点的配置都要加到entries里，然后作为一个日志条目添加到log，还是数个日志条目？
// 感觉像是几个。因为每个conf都会生成一个Entry
    for (size_t i = 0; i < peers.size(); ++i) {
      auto& peer = peers[i];
      // 创建一个配置变更对象，用于添加节点。
      proto::ConfChange cs = proto::ConfChange{
          .id = 0,
          .conf_change_type = proto::ConfChangeAddNode,
          .node_id = peer.id,
          .context = peer.context,
      };
      // 序列化配置变更对象
      std::vector<uint8_t> data = cs.serialize();
      // 创建一个新的日志条目
      proto::EntryPtr entry(new proto::Entry());
      entry->type = proto::EntryConfChange;
      entry->term = 1;
      entry->index = i + 1;
      entry->data = std::move(data);
      entries.push_back(entry);
    }
    // 将初始的配置变更条目追加到Raft日志中
    raft_->raft_log_->append(entries);
    raft_->raft_log_->committed_ = entries.size();

    for (auto& peer : peers) {
      raft_->add_node(peer.id);
    }
  }

  // Set the initial hard and soft states after performing all initialization.
  prev_soft_state_ = raft_->soft_state();
  if (last_index == 0) {
    prev_hard_state_ = proto::HardState();
  } else {
    prev_hard_state_ = raft_->hard_state();
  }
}
/**
 * 恢复现有节点：如果 RawNode 是在恢复一个已经存在的节点（从持久化存储中恢复状态），
 * 那么它只需要知道当前的配置和状态，而不需要初始节点列表。
 */
RawNode::RawNode(const Config& conf) {

  uint64_t last_index = 0;
  Status status = conf.storage->last_index(last_index);
  if (!status.is_ok()) {
    LOG_FATAL("%s", status.to_string().c_str());
  }

  raft_ = std::make_shared<Raft>(conf);

  // Set the initial hard and soft states after performing all initialization.
  prev_soft_state_ = raft_->soft_state();
  if (last_index == 0) {
    prev_hard_state_ = proto::HardState();
  } else {
    prev_hard_state_ = raft_->hard_state();
  }
}

/** 处理定时时间，如心跳和选举超时 */
void RawNode::tick() {
  raft_->tick(); 
}
/** 这个函数负责将Node转为candidate，并开始选举 */
Status RawNode::campaign() {
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgHup;
  return raft_->step(std::move(msg));
}
/** 这个函数用于向Raft实例提出一个新的提案。这个函数将数据封装成一个Raft消息。并传递给Raft实例处理 */
Status RawNode::propose(std::vector<uint8_t> data) {
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgProp;
  msg->from = raft_->id_;
  // EntryNormal就是封装正常日志条目的函数
  msg->entries.emplace_back(proto::EntryNormal, 0, 0, std::move(data));

  return raft_->step(std::move(msg));
}
// 这个函数用于提议状态变更。
Status RawNode::propose_conf_change(const proto::ConfChange& cc) {
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgProp;
  msg->entries.emplace_back(proto::EntryConfChange, 0, 0, cc.serialize());
  return raft_->step(std::move(msg));
}
/**
 * 这个函数的作用是处理传入的消息，并根据消息的类型和来源节点进行适当的处理。他会忽略掉不期望的
 * 本地消息，处理网络消息，并在必要时返回错误状态。
 */
Status RawNode::step(proto::MessagePtr msg) {
  // ignore unexpected local messages receiving over network
  if (msg->is_local_msg()) {
    return Status::invalid_argument("raft: cannot step raft local message");
  }
// 尝试获取消息来源节点的进度信息（Progress对象）。如果找到消息来源节点的进度信息不为空
// or消息不是响应消息，则调用raft_处理该消息
  ProgressPtr progress = raft_->get_progress(msg->from);
  if (progress || !msg->is_response_msg()) {
    return raft_->step(msg);
  }
  return Status::invalid_argument("raft: cannot step as peer not found");
}
/** 生成一个Ready 对象，该对象包括了当前Raft节点的状态信息，并清理和更新了一些状态。
 * Ready对象常用于将Raft的状态变化传递给上层应用或系统，以便进行持久化，通信或应用状态的更新
 */
ReadyPtr RawNode::ready() {
  ReadyPtr rd = std::make_shared<Ready>(raft_, prev_soft_state_, prev_hard_state_);
  raft_->msgs_.clear();
  /** 减少未提交日志的大小（只是减少大小吗？），防止日志无限增长 */
  raft_->reduce_uncommitted_size(rd->committed_entries);
  return rd;
}

/** 检查Raft节点是否有新的状态或数据需要上层系统处理。它通过检查各种状态变化和待处理的数据，
 * 判断是否需要生成一个新的Ready对象。函数返回一个bool值，指示是否有新的准备就绪的数据或状态
 */
bool RawNode::has_ready() {
  assert(prev_soft_state_);
  if (!raft_->soft_state()->equal(*prev_soft_state_)) {
    return true; // true就是有东西要更新
  }
  proto::HardState hs = raft_->hard_state();
  if (!hs.is_empty_state() && !hs.equal(prev_hard_state_)) {
    return true;
  }

  proto::SnapshotPtr snapshot = raft_->raft_log_->unstable_->snapshot_;

  if (snapshot && !snapshot->is_empty()) {
    return true;
  }
  if (!raft_->msgs_.empty() || !raft_->raft_log_->unstable_entries().empty()
      || raft_->raft_log_->has_next_entries()) {
    return true;
  }

  return !raft_->read_states_.empty();
}

/** 推进Raft节点的状态，以便处理Ready对象中的已提交或已应用的日志条目，快照和杜状态。通过调用
 * 这个函数，Raft节点可以更新内部状态，为下一次Ready作准备 
 */
void RawNode::advance(ReadyPtr rd) {
  // 如果Ready中的软状态不为空，则更新RawNode的前一个软状态。软状态通常包括节点是否leader和leader ID
  if (rd->soft_state) {
    prev_soft_state_ = rd->soft_state;

  }
  // hard state包括当前任期，投票信息，和已提交的日志索引。
  if (!rd->hard_state.is_empty_state()) {
    prev_hard_state_ = rd->hard_state;
  }

  // If entries were applied (or a snapshot), update our cursor for
  // the next Ready. Note that if the current HardState contains a
  // new Commit index, this does not mean that we're also applying
  // all of the new entries due to commit pagination by size.
  // 获取Ready已经应用的日志条目的索引。为了简化比较，确保index大于0即可，就是有效的。而在
  // applied to函数中才会检查最后一个已经应用的索引是什么，而不是在advance函数中实现。
  // 如果索引大于0，则更新Raft日志，使其应用到该索引。这一步确保Raft知道哪些条目已经被上层系统（APP）应用
  uint64_t index = rd->applied_cursor();
  if (index > 0) {
    raft_->raft_log_->applied_to(index);
  }

  if (!rd->entries.empty()) {
    auto& entry = rd->entries.back();
    raft_->raft_log_->stable_to(entry->index, entry->term);
  }

  if (!rd->snapshot.is_empty()) {
    raft_->raft_log_->stable_snap_to(rd->snapshot.metadata.index);
  }
// 清空raft 节点 的读状态。确保Raft节点可以处理新的读请求。
  if (!rd->read_states.empty()) {
    raft_->read_states_.clear();
  }
}
/** 应用一个配置变更请求，从而调整Raft集群的成员列表。配置变更请求包括添加节点，添加学习者节点
 * 移除节点，和更新节点等操作。函数返回一个包含当前节点和学习者节点列表的confstate 对象。
 */
proto::ConfStatePtr RawNode::apply_conf_change(const proto::ConfChange& cc) {
  proto::ConfStatePtr state(new proto::ConfState());
  // 如果请求变更节点ID是0，仅获取当前节点和学习者节点列表。
  if (cc.node_id == 0) {
    raft_->nodes(state->nodes); // 将当前node加载到state的node信息中
    raft_->learner_nodes(state->learners);
    return state;
  }

  switch (cc.conf_change_type) {
    case proto::ConfChangeAddNode: {
      raft_->add_node_or_learner(cc.node_id, false);
      break;
    }
    case proto::ConfChangeAddLearnerNode: {
      raft_->add_node_or_learner(cc.node_id, true);
      break;
    }
    case proto::ConfChangeRemoveNode: {
      raft_->remove_node(cc.node_id);
      break;
    }
    case proto::ConfChangeUpdateNode: {
      LOG_DEBUG("ConfChangeUpdate");
      break;
    }
    default: {
      LOG_FATAL("unexpected conf type");
    }
  }
  raft_->nodes(state->nodes);
  raft_->learner_nodes(state->learners);
  return state;
}

void RawNode::transfer_leadership(uint64_t lead, ino64_t transferee) {
  // manually set 'from' and 'to', so that leader can voluntarily transfers its leadership
  proto::MessagePtr msg(new proto::Message()); // 以下三步就是把领导权转换的过程封装到msg里
  msg->type = proto::MsgTransferLeader;
  msg->from = transferee; // transferee是旧领导
  msg->to = lead; // to是新领导。leadership从旧领导转移到新领导

  Status status = raft_->step(std::move(msg));
  if (!status.is_ok()) {
    LOG_WARN("transfer_leadership %s", status.to_string().c_str());
  }
}
/**
 * 这个函数用于在分布式系统中通过Raft协议确保读取操作的一致性和安全性。通过read-index请求，系统
 * 可以确定安全读取数据的位置，并保证读取操作是线性化的，即按照请求的顺序处理。
 */
Status RawNode::read_index(std::vector<uint8_t> rctx) {
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgReadIndex;
  msg->entries.emplace_back(proto::MsgReadIndex, 0, 0, std::move(rctx));
  return raft_->step(std::move(msg));
}
// 报告Raft 的状态
RaftStatusPtr RawNode::raft_status() {
  LOG_DEBUG("no impl yet");
  return nullptr;
}
// 报告最后发送消息的这个node（id指代）找不到
void RawNode::report_unreachable(uint64_t id) {
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgUnreachable;
  msg->from = id;

  Status status = raft_->step(std::move(msg));
  if (!status.is_ok()) {
    LOG_WARN("report_unreachable %s", status.to_string().c_str());
  }
}
/** 报告快照的状态。 */
void RawNode::report_snapshot(uint64_t id, SnapshotStatus status) {
  bool rej = (status == SnapshotFailure); // 检查传入的快照状态是否为Sanpshot Failure
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgSnapStatus; // 表示这是一个快照状态消息
  msg->from = id; // 表示这是哪个节点报告的快照状态
  msg->reject = rej; // 表示快照是否失败

  Status s = raft_->step(std::move(msg));
  if (!s.is_ok()) {
    LOG_WARN("report_snapshot %s", s.to_string().c_str());
  }
}

void RawNode::stop() {

}

}