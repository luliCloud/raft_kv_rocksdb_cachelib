#include <raft-kv/raft/storage.h>
#include <raft-kv/common/log.h>
#include <raft-kv/raft/util.h>
/**
 * 存在MemoryStorage实例中的成员变量（hard_state_, snapshot_, etc）起到了缓存的作用。
 * 存储了当前的状态，这些数据可以快速访问，不需要每次都从外部存储中读取。
 */
namespace kv {
// 将硬状态和snapshot等存到WAL（现在应该是初始状态。在construct一个新的Memory Storage instance后
// 就会有这些成员的初始值）。如果要更新这些值（在KV写入的过程中，则会有set_hard_state 等函数）
Status MemoryStorage::initial_state(proto::HardState& hard_state, proto::ConfState& conf_state) {
  hard_state = hard_state_;

  // copy
  conf_state = snapshot_->metadata.conf_state;
  return Status::ok();
}

void MemoryStorage::set_hard_state(proto::HardState& hard_state) {
  std::lock_guard<std::mutex> guard(mutex_);
  hard_state_ = hard_state;
}
/**
 * 在WAL-FILE没有使用mutex但是在storage中使用了mutex 
 * 使用 mutex 来保护内存中的数据结构是为了确保多线程访问的线程安全性。而在处理磁盘上的文件时，
 * 通常会依赖文件系统本身的机制来确保一致性。
 */
Status MemoryStorage::entries(uint64_t low,
                              uint64_t high,
                              uint64_t max_size,
                              std::vector<proto::EntryPtr>& entries) {
  assert(low < high);
  std::lock_guard<std::mutex> guard(mutex_);

  uint64_t offset = entries_[0]->index;
  if (low <= offset) { // 如果日志范围的low 小于等于日志条目0的index，invalid （最小日志条目已经被合并）
    return Status::invalid_argument("requested index is unavailable due to compaction");
  }
  uint64_t last = 0;
  this->last_index_impl(last); // 最后一个日志条目

  if (high > last + 1) {
    LOG_FATAL("entries' hi(%lu) is out of bound last_index(%lu)", high, last);
  }
  // only contains dummy entries. 在初始化时就在term 0插入了 dummy entries
  if (entries_.size() == 1) {
    return Status::invalid_argument("requested entry at index is unavailable");
  }
// i是从entries的第1条（0是dummy entry）开始遍历。因为entries[1] 的index不一定是0.所以要计算这个差值对应从第几条起
  for (uint64_t i = low - offset; i < high - offset; ++i) {
    entries.push_back(entries_[i]);
  }
  entry_limit_size(max_size, entries);
  return Status::ok();
}
// 返回要求条目的日志条目的任期，并更新传入的term 变量
Status MemoryStorage::term(uint64_t i, uint64_t& term) {
  std::lock_guard<std::mutex> guard(mutex_);

  uint64_t offset = entries_[0]->index;

  if (i < offset) {
    return Status::invalid_argument("requested index is unavailable due to compaction");
  }

  if (i - offset >= entries_.size()) {
    return Status::invalid_argument("requested entry at index is unavailable");
  }
  term = entries_[i - offset]->term;
  return Status::ok();
}
// 返回最后storage中的最后一个日志条目 （所有条目存在entries_ 成员变量里）
Status MemoryStorage::last_index(uint64_t& index) {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_index_impl(index);
}

Status MemoryStorage::first_index(uint64_t& index) {
  std::lock_guard<std::mutex> guard(mutex_);
  return first_index_impl(index);
}

Status MemoryStorage::snapshot(proto::SnapshotPtr& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);
  snapshot = snapshot_;
  return Status::ok();
}

Status MemoryStorage::compact(uint64_t compact_index) {
  std::lock_guard<std::mutex> guard(mutex_);
// entries_[0] 是一个虚拟的（dummy）条目，用于保存最新的压缩点之前的最后一个条目的索引和任期。
  uint64_t offset = entries_[0]->index;

  if (compact_index <= offset) {
    return Status::invalid_argument("requested index is unavailable due to compaction");
  }

  uint64_t last_idx;
  this->last_index_impl(last_idx);
  if (compact_index > last_idx) {
    LOG_FATAL("compact %lu is out of bound lastindex(%lu)", compact_index, last_idx);
  }
// 找到需要截断的位置。将entries0 的index和term设置为指定日志的前一个的索引和任期。entries0是dummy，需要保留。
// 同时indicate
// 0,1,2,3,4,5. // request compact idx = 5
// i = 5;
// entries[0] = entires[5] = 4; -> 4 -> 4,5
// 4，5，6，7 // compactidx= 6
// i = 6 - 4 = 2;
// entires[0] = entires[2] = 6 -> 6,7
  uint64_t i = compact_index - offset;
  entries_[0]->index = entries_[i]->index;
  entries_[0]->term = entries_[i]->term;
// erase begin+1到begin+i的位置，不包括再+1
  entries_.erase(entries_.begin() + 1, entries_.begin() + i + 1);
  return Status::ok();
}
/** 将一组新的日志条目追击到内存存储中（MemoryStorage） */
Status MemoryStorage::append(std::vector<proto::EntryPtr> entries) {
  if (entries.empty()) { // 如果没有日志条目，就直接返回Status：：OK
    return Status::ok();
  }

  std::lock_guard<std::mutex> guard(mutex_);

  uint64_t first = 0; 
  first_index_impl(first); // 将first设成现有条目的索引（应该是memory中已有的第一个条目，不是最后一个）
  uint64_t last = entries[0]->index + entries.size() - 1; // 得到最后一个日志条目的index

  // shortcut if there is no new entry.如果没有新条目要增加，直接返回。
  // 逻辑是最后一个要追加的条目都比现有的条目要小
  if (last < first) {
    return Status::ok();
  }

  // truncate compacted entries 如果新条目的第一个索引小鱼现有条目的第一个索引，删除新条目中已经被压缩的部分
  if (first > entries[0]->index) {
    uint64_t n = first - entries[0]->index;
    // first 之前的 entry 已经进入 snapshot, 丢弃
    entries.erase(entries.begin(), entries.begin() + n);
  }
// 计算新条目相对于现有条目的偏移量
  uint64_t offset = entries[0]->index - entries_[0]->index;

  if (entries_.size() > offset) {
    //MemoryStorage [first, offset] 被保留, offset 之后的丢弃，
    entries_.erase(entries_.begin() + offset, entries_.end());
    entries_.insert(entries_.end(), entries.begin(), entries.end()); // 插入新的条目中的部分
  } else if (entries_.size() == offset) { // 刚好等于偏移量。不需要截断
    entries_.insert(entries_.end(), entries.begin(), entries.end());
  } else { // 现有条目小于偏移量，有日志缺失。fatal error
    uint64_t last_idx;
    last_index_impl(last_idx);
    LOG_FATAL("missing log entry [last: %lu, append at: %lu", last_idx, entries[0]->index);
  }
  return Status::ok();
}
/** 根据请求的索引创建某个节点的快照 */
Status MemoryStorage::create_snapshot(uint64_t index,
                                      proto::ConfStatePtr cs,
                                      std::vector<uint8_t> data,
                                      proto::SnapshotPtr& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);
// 检查请求的索引是否比现有快照的索引更旧。如果更旧，创建一个新的空快照并返回错误状态
  if (index <= snapshot_->metadata.index) {
    snapshot = std::make_shared<proto::Snapshot>();
    return Status::invalid_argument("requested index is older than the existing snapshot");
  }

  uint64_t offset = entries_[0]->index;
  uint64_t last = 0;
  last_index_impl(last);
  if (index > last) { // 如果请求的索引比memory中的最后一个日志条目的索引还大
    LOG_FATAL("snapshot %lu is out of bound lastindex(%lu)", index, last);
  }

  snapshot_->metadata.index = index;
  snapshot_->metadata.term = entries_[index - offset]->term;
  if (cs) {
    snapshot_->metadata.conf_state = *cs;  // 更新快照的配置状态，如果存在
  }
  snapshot_->data = std::move(data); // 将数据移动到快照
  snapshot = snapshot_; // 更新快照指针
  return Status::ok();

}
// 该方法使用一个快照，更新内存存储中状态，以反应快照中的数据。
Status MemoryStorage::apply_snapshot(const proto::Snapshot& snapshot) {
  std::lock_guard<std::mutex> guard(mutex_);

  uint64_t index = snapshot_->metadata.index; // 内存中目前快照的index
  uint64_t snap_index = snapshot.metadata.index; // 传入的快照的index

  if (index >= snap_index) {  // 如果内存中本身的快照idx比传入的大，不需要更新
    return Status::invalid_argument("requested index is older than the existing snapshot");
  }

  snapshot_ = std::make_shared<proto::Snapshot>(snapshot); // 使内存中的快照指针指向传进来的快照
// 调整日志
  entries_.resize(1); // 将日志条目调整为一条
  proto::EntryPtr entry(new proto::Entry());
  entry->term = snapshot_->metadata.term; // 内存中前面的entry都可以去掉了。只留下对应新快照的这条日志
  entry->index = snapshot_->metadata.index;
  entries_[0] = std::move(entry); // 作为dummy起始点。
  return Status::ok();
}
// memory中最后一条目录的index （绝对值）
Status MemoryStorage::last_index_impl(uint64_t& index) {
  index = entries_[0]->index + entries_.size() - 1;
  return Status::ok();
}

Status MemoryStorage::first_index_impl(uint64_t& index) {
  index = entries_[0]->index + 1;
  return Status::ok();
}

}