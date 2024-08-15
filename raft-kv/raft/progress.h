#pragma once
#include <memory>
#include <vector>

namespace kv {
/* ProgressState Probe： 节点处理探测状态，领导者会以心跳间隔发送最多一条复制消息，并探测跟随者的实际进展
ProgressStateReplicate： 节点处于复制状态，领导者会乐观的增加next，以快速向跟随者复制日志条目
ProgressStateSnapshot：节点处于快照状态，此时领导者已经发送了快照，并停止发送复制消息 */
enum ProgressState {
  ProgressStateProbe = 0,
  ProgressStateReplicate = 1,
  ProgressStateSnapshot = 2
};

const char* progress_state_to_string(ProgressState state);

/** InFlights class 用于管理已经发送但还未确认的日志条目。
 * start：当前缓冲区的起始索引，count：缓冲区中正在飞行的消息数。size：缓冲区的最大容量。
 * buffer：存储正在飞行中的消息索引的圆形缓冲区
 */
class InFlights {
 public:
  explicit InFlights(uint64_t max_inflight_msgs)
      : start(0),
        count(0),
        size(static_cast<uint32_t>(max_inflight_msgs)) {}
/* 重置滑动窗口，清空正在飞行中的消息*/
  void reset() {
    start = 0;
    count = 0;
  }
/* 判断缓冲区是否已满 */
  bool is_full() const {
    return size == count;
  }
/* 添加一个飞行消息到缓冲区 */
  void add(uint64_t inflight);

  // freeTo frees the inflights smaller or equal to the given `to` flight.
  /* 释放小于或等于to 的飞行消息*/
  void free_to(uint64_t to);
/* 释放第一个 飞行消息*/
  void free_first_one();

  // the starting index in the buffer
  uint32_t start;
  // number of inflights in the buffer
  uint32_t count;

  // the size of the buffer
  uint32_t size;

  // ring buffer contains the index of the last entry
  // inside one message.
  std::vector<uint64_t> buffer;
};

// Progress represents a follower’s progress in the view of the leader. Leader maintains
// progresses of all followers, and sends entries to the follower based on its progress.
/* Progress 类表示一个跟随者在领导者视角下的进展情况。领导者会维护所有跟随者的进展，
并基于这些信息决定如何向跟随者发送日志条目。
match：表示该节点已经成功复制的log idx。next：表示下一个待复制的log idx。state：当前的进展状态，可能是
ProgressStateProbe、ProgressStateReplicate 或 ProgressStateSnapshot。
paused：标记该节点是否暂停接受复制消息。pending- snapshot：当前挂起的快照索引，如果该值不为0，表示该节点
正在等待快照完成。 recent-active：表示该节点最近是否活跃。
inflight：一个inflight的对象，管理当前正在飞行中的消息。
is- learner：白噢时该节点是否是一个学习者
*/
class Progress {
 public:
  explicit Progress(uint64_t max_inflight)
      : match(0),
        next(0),
        state(ProgressState::ProgressStateProbe),
        paused(false),
        pending_snapshot(0),
        recent_active(false),
        inflights(new InFlights(max_inflight)),
        is_learner(false) {

  }
/* become 都是用于切换节点的状态 */
  void become_replicate();

  void become_probe();

  void become_snapshot(uint64_t snapshoti);
/* 重置节点的状态 */
  void reset_state(ProgressState state);
/* 返回当前节点的状态的字符串表示*/
  std::string string() const;

  bool is_paused() const;

  void set_pause() {
    this->paused = true;
  }

  void resume() {
    this->paused = false;
  }
/* 更新节点的进展。如果给定的索引n比当前进展更新一点，则返回true */
  // maybe_update returns false if the given n index comes from an outdated message.
  // Otherwise it updates the progress and returns true.
  bool maybe_update(uint64_t n);
/* 乐观的更新next 为n+1*/
  void optimistic_update(uint64_t n) {
    next = n + 1;
  }
/* 如果给定的rejected索引来自乱序消息，返回false，否则更新next为min（rejected，last）  */
  // maybe_decr_to returns false if the given to index comes from an out of order message.
  // Otherwise it decreases the progress next index to min(rejected, last) and returns true.
  bool maybe_decreases_to(uint64_t rejected, uint64_t last);

  // need_snapshot_abort returns true if snapshot progress's match
  // is equal or higher than the pending_snapshot.
  bool need_snapshot_abort() const;

  void snapshot_failure() {
    pending_snapshot = 0;
  }

  uint64_t match;
  uint64_t next;
  // state defines how the leader should interact with the follower.
  //
  // When in ProgressStateProbe, leader sends at most one replication message
  // per heartbeat interval. It also probes actual progress of the follower.
  //
  // When in ProgressStateReplicate, leader optimistically increases next
  // to the latest entry sent after sending replication message. This is
  // an optimized state for fast replicating log entries to the follower.
  //
  // When in ProgressStateSnapshot, leader should have sent out snapshot
  // before and stops sending any replication message.
  ProgressState state;

  // paused is used in ProgressStateProbe.
  // When Paused is true, raft should pause sending replication message to this peer.
  bool paused;
  // pending_snapshot is used in ProgressStateSnapshot.
  // If there is a pending snapshot, the pendingSnapshot will be set to the
  // index of the snapshot. If pendingSnapshot is set, the replication process of
  // this Progress will be paused. raft will not resend snapshot until the pending one
  // is reported to be failed.
  uint64_t pending_snapshot;

  // recent_active is true if the progress is recently active. Receiving any messages
  // from the corresponding follower indicates the progress is active.
  // recent_active can be reset to false after an election timeout.
  bool recent_active;


  // inflights is a sliding window for the inflight messages.
  // Each inflight message contains one or more log entries.
  // The max number of entries per message is defined in raft config as MaxSizePerMsg.
  // Thus inflight effectively limits both the number of inflight messages
  // and the bandwidth each Progress can use.
  // When inflights is full, no more message should be sent.
  // When a leader sends out a message, the index of the last
  // entry should be added to inflights. The index MUST be added
  // into inflights in order.
  // When a leader receives a reply, the previous inflights should
  // be freed by calling inflights.freeTo with the index of the last
  // received entry.

  std::shared_ptr<InFlights> inflights;

  // is_learner is true if this progress is tracked for a learner.
  bool is_learner;

};
typedef std::shared_ptr<Progress> ProgressPtr;

}