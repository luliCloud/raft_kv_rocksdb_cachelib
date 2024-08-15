#include <raft-kv/raft/progress.h>
#include <raft-kv/common/log.h>

namespace kv {

const char* progress_state_to_string(ProgressState state) {
  switch (state) {
    case ProgressStateProbe: {
      return "ProgressStateProbe";
    }
    case ProgressStateReplicate: {
      return "ProgressStateReplicate";
    }
    case ProgressStateSnapshot: {
      return "ProgressStateSnapshot";
    }
    default: {
      LOG_FATAL("unknown state %d", state);
    }
  }
}

void InFlights::add(uint64_t inflight) {
  if (is_full()) {
    LOG_FATAL("cannot add into a full inflights");
  }

  uint64_t next = start + count;

  if (next >= size) {
    next -= size;  // buffer是一个环形的？
  }
  if (next >= buffer.size()) {  // buffer can be resized
    uint32_t new_size = buffer.size() * 2;
    if (new_size == 0) {
      new_size = 1;
    } else if (new_size > size) {
      new_size = size;
    }
    buffer.resize(new_size);
  }
  buffer[next] = inflight;
  count++;
}
/** 注意这里比较的是to和buffer中idx指向的日志的idx，也就是我们会把buffer中小于等于to 的所有日志idx删掉
 * 而不是将buffer中所有idx小于to的日志删掉。也就是说，这个to是对应日志的idx绝对值，而不是对应buffer位置的相对值
 */
void InFlights::free_to(uint64_t to) {
  if (count == 0 || to < buffer[start]) {
    // out of the left side of the window
    return;
  }

  uint32_t idx = start;
  size_t i;
  for (i = 0; i < count; i++) {
    if (to < buffer[idx]) { // found the first large inflight。idx是在buffer中的index
    // 因为有可能全都小于to或者最小的都比to大，所以不能直接用to
      break;
    }

    // increase index and maybe rotate
    idx++;

    if (idx >= size) { // 这里会重置idx到开头
      idx -= size;
    }
  }
  // free i inflights and set new start index。i个flight都要被删掉
  // 注意重新调整的仅仅是count，用i计算出来。和start，标记我们的数据从哪里开始。用这两个变量就足以找到
  // 我们的valid data 在环形buffer中从哪里开始，在哪里结束（即使走到了buffer的尽头）
  count -= i;
  start = idx;
  if (count == 0) {
    // inflights is empty, reset the start index so that we don't grow the
    // buffer unnecessarily.
    start = 0;
  }
}

void InFlights::free_first_one() {
  free_to(buffer[start]);
}

void Progress::become_replicate() {
  reset_state(ProgressStateReplicate);
  next = match + 1;
}

void Progress::become_probe() {
  // If the original state is ProgressStateSnapshot, progress knows that
  // the pending snapshot has been sent to this peer successfully, then
  // probes from pendingSnapshot + 1.
  /* 如果ProgressState Snapshot状态，表示这个pending snapshot（idx）已经被成功发送给follower
  注意follower不一定应用成功。但是leader已经发出去。那么leader会乐观的重置状态为Probe并发送
  下一个更新的snapshot */
  if (state == ProgressStateSnapshot) {
    uint64_t pending = pending_snapshot;
    reset_state(ProgressStateProbe);
    next = std::max(match + 1, pending + 1); // log+1， pending-snapshot + 1
  } else {
    reset_state(ProgressStateProbe);
    next = match + 1;
  }
}

void Progress::become_snapshot(uint64_t snapshoti) {
  reset_state(ProgressStateSnapshot);
  pending_snapshot = snapshoti;
}

void Progress::reset_state(ProgressState st) {
  paused = false;
  pending_snapshot = 0;
  this->state = st;
  this->inflights->reset();
}

std::string Progress::string() const {
  char buffer[256];
  int n = snprintf(buffer,
                   sizeof(buffer),
                   "next = %lu, match = %lu, state = %s, waiting = %d, pendingSnapshot = %lu",
                   next,
                   match,
                   progress_state_to_string(state),
                   is_paused(),
                   pending_snapshot);
  return std::string(buffer, n);
}

bool Progress::is_paused() const {
  switch (state) {
    case ProgressStateProbe: {
      return paused;
    }
    case ProgressStateReplicate: {
      return inflights->is_full();
    }
    case ProgressStateSnapshot: {
      return true;
    }
    default: {
      LOG_FATAL("unexpected state");
    }
  }
}
/** 通过比较match和n判断是否需要更新。如果match小于n，那么就要更新到n。如果next小于n+1，就要更新next */
bool Progress::maybe_update(uint64_t n) {
  bool updated = false;
  if (match < n) {
    match = n;
    updated = true;
    resume();
  }
  if (next < n + 1) {
    next = n + 1;
  }
  return updated;
}
/** Progress::maybe_decreases_to 是一个用于调整 next 索引的函数，在分布式共识算法（如 Raft）中，
 * 这个函数的作用是在某些情况下减少 next 索引，以便领导者（Leader）可以重新尝试将日志条目复制到追随者。
 * 具体来说，当追随者拒绝了某个日志条目的复制时，领导者可能需要回退 next 索引并重新发送日志条目。 
 * next: 领导者（Leader）认为追随者（Follower）下一个应接收的日志条目的索引。
match: 追随者已经成功复制并确认的最高日志条目的索引。
rejected: 追随者拒绝的日志条目的索引，通常是follower在处理leader发送的log时发现日志在这个idx位置上与leader不一致。
*/
bool Progress::maybe_decreases_to(uint64_t rejected, uint64_t last) {
  if (state == ProgressStateReplicate) {
    // Leader处于快速复制状态，意味着leader正在尝试将日志尽可能复制到Follower
    // the rejection must be stale if the progress has matched and "rejected"
    // is smaller than "match".
    if (rejected <= match) {  // 这个拒绝是过时的，不需要回退索引
      return false;
    }
    // directly decrease next to match + 1。如果rejected大于match，那么回退next到match+1
    // 并准备重新发送match的后续日志
    next = match + 1;
    return true;
  }
  // Part2:处理非Replicate状态
  // the rejection must be stale if "rejected" does not match next - 1
  if (next - 1 != rejected) { // 在非replicate的状态下，如果拒绝与next-1不匹配，认为这个拒绝是过时
    return false;
  }
// 更新next索引。next要么设置为被拒绝的索引，要么设置为follower已知的最后一个log+1
  next = std::min(rejected, last + 1);
  if (next < 1) {
    next = 1;
  }
  resume();
  return true;
}

bool Progress::need_snapshot_abort() const {
  return state == ProgressStateSnapshot && match >= pending_snapshot;
}

}
