#pragma once
#include <memory>
#include <mutex>
#include <raft-kv/common/status.h>
#include <raft-kv/raft/proto.h>

namespace kv {

/**
 * Storage 是抽象基类，定义累一系列管理Raft日志和快照的纯虚函数接口。
 * Memory Storage继承自Storage，并实现了这些借口。使用内存数据作为后端存储。
 */
class Storage {
 public:
  ~Storage() = default;

  // initial_state returns the saved hard_state and ConfState information.
  virtual Status initial_state(proto::HardState& hard_state, proto::ConfState& conf_state) = 0;

  // entries returns a slice of log entries in the range [low,high).
  // MaxSize limits the total size of the log entries returned, but
  // entries returns at least one entry if any.
  // 返回指定范围的日志条目。low high是日志条目索引范围
  virtual Status entries(uint64_t low,
                         uint64_t high,
                         uint64_t max_size,
                         std::vector<proto::EntryPtr>& entries) = 0;

  // Term returns the term of entry i, which must be in the range
  // [FirstIndex()-1, LastIndex()]. The term of the entry before
  // FirstIndex is retained for matching purposes even though the
  // rest of that entry may not be available.
  // 返回指定索引的日志条目的任期
  virtual Status term(uint64_t i, uint64_t& term) = 0;

  // LastIndex returns the index of the last entry in the log.
  // 返回日志中最后一个条目的索引
  virtual Status last_index(uint64_t& index) = 0;

  // firstIndex returns the index of the first log entry that is
  // possibly available via entries (older entries have been incorporated
  // into the latest Snapshot; if storage only contains the dummy entry the
  // first log entry is not available).
  virtual Status first_index(uint64_t& index) = 0;

  // Snapshot returns the most recent snapshot.
  // If snapshot is temporarily unavailable, it should return ErrSnapshotTemporarilyUnavailable,
  // so raft state machine could know that Storage needs some time to prepare
  // snapshot and call Snapshot later.
  // 返回最近的快照 （most recent）
  virtual Status snapshot(proto::SnapshotPtr& snapshot) = 0;
};
typedef std::shared_ptr<Storage> StoragePtr;

// MemoryStorage implements the Storage interface backed by an
// in-memory array.
class MemoryStorage : public Storage {
 public:

  // creates an empty MemoryStorage
  explicit MemoryStorage()
      : snapshot_(new proto::Snapshot()) {
    // When starting from scratch populate the list with a dummy entry at term zero.
    // 初始化一个空的Memory Storage对象，并在term为0处插入一个虚拟条目
    proto::EntryPtr entry(new proto::Entry());
    entries_.emplace_back(std::move(entry));
  }

  virtual Status initial_state(proto::HardState& hard_state, proto::ConfState& conf_state);

  void set_hard_state(proto::HardState& hard_state);

  virtual Status entries(uint64_t low,
                         uint64_t high,
                         uint64_t max_size,
                         std::vector<proto::EntryPtr>& entries);

  virtual Status term(uint64_t i, uint64_t& term);

  virtual Status last_index(uint64_t& index);

  virtual Status first_index(uint64_t& index);

  virtual Status snapshot(proto::SnapshotPtr& snapshot);

  // compact discards all log entries prior to compact_index.
  // It is the application's responsibility to not attempt to compact an index
  // greater than raftLog.applied.
  // 丢弃指定索引之前的所有日志条目（不包括compact idx 自己）
  Status compact(uint64_t compact_index);

  // append the new entries to storage.
  // append 追加新的日志条目到存储中
  Status append(std::vector<proto::EntryPtr> entries);

  // create_snapshot makes a snapshot which can be retrieved with Snapshot() and
  // can be used to reconstruct the state at that point.
  // If any configuration changes have been made since the last compaction,
  // the result of the last apply_conf_change must be passed in.
  // 创建一个新的快照，可用于在指定点恢复状态
  Status create_snapshot(uint64_t index,
                         proto::ConfStatePtr cs,
                         std::vector<uint8_t> data,
                         proto::SnapshotPtr& snapshot);

  // ApplySnapshot overwrites the contents of this Storage object with
  // those of the given snapshot.
  // 使用指定的快照覆盖当前存储的内容
  Status apply_snapshot(const proto::Snapshot& snapshot);
 public:
  Status last_index_impl(uint64_t& index);
  Status first_index_impl(uint64_t& index);

  std::mutex mutex_;
  proto::HardState hard_state_;
  proto::SnapshotPtr snapshot_;
  // entries_[i] has raft log position i+snapshot.Metadata.Index
  std::vector<proto::EntryPtr> entries_;
};
typedef std::shared_ptr<MemoryStorage> MemoryStoragePtr;

}
/**
 * storage 的设计主要是为了提供一种方便、高效的方式，从内存中读取最新的一部分日志条目，并且在需要的时候快速访问这些数据。让我们进一步探讨这个设计的具体细节和优势。

storage 的具体作用
快速访问最新日志条目：

内存存储：通过将最新的日志条目保存在内存中，storage 提供了快速访问这些条目的能力。内存访问的速度远快于磁盘访问，这使得读取和处理最新日志条目的操作更加高效。
减少延迟：在分布式系统中，减少延迟对于提高系统性能非常重要。通过使用内存存储，系统可以在极低的延迟下读取最新的日志条目。
方便的日志管理：

统一接口：storage 提供了一个统一的接口来管理日志条目，包括追加日志、读取日志、获取日志条目的任期等。这使得日志管理更加方便和灵活。
易于扩展：通过抽象的 Storage 接口，系统可以轻松地切换不同的存储实现（如 MemoryStorage 或 DiskStorage），以适应不同的应用需求。
结合持久化机制：

持久化存储（WAL）：虽然 storage 主要用于内存中的快速访问，但结合持久化存储（如 WAL），可以确保数据的可靠性和持久性。在系统崩溃或重启时，可以从 WAL 中恢复数据。
快照机制：系统可以定期创建快照，将系统状态持久化到磁盘，从而减少日志条目的数量和恢复时间。
 */
