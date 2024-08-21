#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <future>
#include <raft-kv/server/raft_node.h>
#include <raft-kv/common/log.h>

/**
 * 在一个 C++ 文件中使用 namespace kv 包括所有变量和函数实现有以下几个主要原因：
避免命名冲突：
命名空间可以有效地防止命名冲突。当项目变得庞大时，不同模块可能会有相同名字的变量或函数。
通过将这些变量和函数放入不同的命名空间中，可以确保它们之间不会发生冲突​ (CppReference)​。
 */
namespace kv {
// uint64_t, 无符号64位整型
// 这些类型通过 typedef 定义，是标准整数类型的别名，确保在不同平台上具有相同的大小和语义。
// 例如，在 32 位和 64 位系统上，uint64_t 始终是 64 位长的无符号整数​
static uint64_t defaultSnapCount = 100000;
static uint64_t snapshotCatchUpEntriesN = 100000;
// static uint64_t defaultSnapCount = 3;
// static uint64_t snapshotCatchUpEntriesN = 2;

// RaftNode:: is a class. 这三个变量从raft-kv的main()读进来. constructor for RaftNode class
RaftNode::RaftNode(uint64_t id, const std::string& cluster, uint16_t port)
    : port_(port),
      pthread_id_(0),
      timer_(io_service_),
      id_(id),
      last_index_(0),
      conf_state_(new proto::ConfState()),
      snapshot_index_(0),
      applied_index_(0),
      storage_(new MemoryStorage()),
      snap_count_(defaultSnapCount) {
  // boost is a namespace. boost::split 是 Boost 库中的一个函数，用于将字符串分割成多个子字符串。
  // 这里通过“，”划分cluster （cluster 是数个string由“，”分开）。peers接读到的cluster的内容
  // peers_ 定义在header里，是vector<string>
  boost::split(peers_, cluster, boost::is_any_of(","));
  if (peers_.empty()) { // peers_存储整个集群的情况，该RaftNode和集群其他的节点
    LOG_FATAL("invalid args %s", cluster.c_str());
  }

  std::string work_dir = "node_" + std::to_string(id);
  snap_dir_ = work_dir + "/snap";
  wal_dir_ = work_dir + "/wal";

  // /** Rocksdb: create rocksdb db based on node id, noting word_dir already node specific */
  // rocksdb_dir_ = work_dir + "/db";
  // rocksdb::Options options;
  // options.create_if_missing = true;
  // /** create this path not exist */
  // if (!boost::filesystem::exists(rocksdb_dir_)) {
  //   boost::filesystem::create_directories(rocksdb_dir_);
  // }
  // // /** to find the db location */
  // // boost::filesystem::path abs_work_dir = boost::filesystem::absolute(rocksdb_dir_);
  // // LOG_INFO("Absolute path: %s", abs_work_dir.c_str());

  // rocksdb::Status st = rocksdb::DB::Open(options, rocksdb_dir_, &db_);
  // if (!st.ok()) {
  //   throw std::runtime_error("Failed to open RocksDB: " + st.ToString());
  // }

  if (!boost::filesystem::exists(snap_dir_)) {
    boost::filesystem::create_directories(snap_dir_);
  }

  snapshotter_.reset(new Snapshotter(snap_dir_));

  bool wal_exists = boost::filesystem::exists(wal_dir_);

  replay_WAL();

  Config c;
  c.id = id;
  c.election_tick = 10;
  c.heartbeat_tick = 1;
  c.storage = storage_;
  c.applied = 0;
  c.max_size_per_msg = 1024 * 1024;
  c.max_committed_size_per_ready = 0;
  c.max_uncommitted_entries_size = 1 << 30;
  c.max_inflight_msgs = 256;
  c.check_quorum = true;
  c.pre_vote = true;
  c.read_only_option = ReadOnlySafe;
  c.disable_proposal_forwarding = false;

  Status status = c.validate(); // status定义在common/status.h里。注意与wal-type区分

  if (!status.is_ok()) {
    LOG_FATAL("invalid configure %s", status.to_string().c_str());
  }

  if (wal_exists) {
    node_.reset(Node::restart_node(c));
  } else {
    std::vector<PeerContext> peers;
    for (size_t i = 0; i < peers_.size(); ++i) {
      peers.push_back(PeerContext{.id = i + 1});
    }
    node_.reset(Node::start_node(c, peers));
  }
}
/** for Rocksdb */
// void RaftNode::deleteDatabase() {
//   delete db_;  // release ptr
//   db_ = nullptr;

//   // destroy database
//   rocksdb::Options options;
//   rocksdb::Status status = rocksdb::DestroyDB(rocksdb_dir_, options);
//   if (!status.ok()) {
//     throw std::runtime_error("Failed to destroy RocksDB: " + status.ToString());
//   }
// }
RaftNode::~RaftNode() {
  LOG_DEBUG("stopped");
  if (transport_) {
    transport_->stop();
    transport_ = nullptr;
  }
}
// 设置一个定时器（可能是心跳机制or反应超时时间）
void RaftNode::start_timer() {
  timer_.expires_from_now(boost::posix_time::millisec(100)); // 设置超时时间100ms
  // async_wait 方法启动一个异步操作，当定时器到期时，传递一个 lambda 函数作为回调。
  // 这个 lambda 函数将在定时器到期时被调用。
  timer_.async_wait([this](const boost::system::error_code& err) {
    if (err) {
      LOG_ERROR("timer waiter error %s", err.message().c_str());
      return;
    }
// 进入第一个async后100ms开始执行下面三行程序
    this->start_timer(); // 如果没有错误，递归调用start-timer重启定时器。
    // 进去后等待100ms，不会阻塞第一个start-timer中的tick和pull
    this->node_->tick(); // 调用tick方法，处理Raft协议的定时任务（心跳信息，候选者发起选举等）
    this->pull_ready_events(); // 处理准备就绪的事件（客户端请求，日志复制）
  });
}

void RaftNode::pull_ready_events() {
  assert(pthread_id_ == pthread_self()); // 确保当前线程是预期的thread
  while (node_->has_ready()) { // 当node有准备好的事件，循环处理
    auto rd = node_->ready(); // 获取准备好的事件
    if (!rd->contains_updates()) { // 如果准备好的事件不包含更新
      LOG_WARN("ready not contains updates");
      return;
    }
// wal_是当前进行管理的WAL（一个以dir和snapshot为基准的WAL instance）
    wal_->save(rd->hard_state, rd->entries); // 将该事件的硬状态和日志条目保存到WAL

    if (!rd->snapshot.is_empty()) { // 如果当前事件的快照不为空
      Status status = save_snap(rd->snapshot); // 保存快照到storage_ （storage是内存中的最新日志条目，保证快速访问）
      if (!status.is_ok()) {
        LOG_FATAL("save snapshot error %s", status.to_string().c_str());
      }
      storage_->apply_snapshot(rd->snapshot); // 应用快照到内存storage
      publish_snapshot(rd->snapshot); // 发布快照：将快照发布给其他节点。
    }

    if (!rd->entries.empty()) { // 如果有新的日志it啊哦亩，追加到存储
      storage_->append(rd->entries);
    }
    if (!rd->messages.empty()) { // 如果有新的消息，调用transport发送消息
      transport_->send(rd->messages);
    }

    if (!rd->committed_entries.empty()) { // 如果有提交的日志，将这些条目转换为可应用的条目，并发布这些条目
      std::vector<proto::EntryPtr> ents;
      entries_to_apply(rd->committed_entries, ents);
      if (!ents.empty()) {
        publish_entries(ents);
      }
    }
    maybe_trigger_snapshot(); // 调用该方法检查是否出要触发新的快照
    node_->advance(rd); // 通知其他节点已经处理好这些准备好的事件
  }
}

Status RaftNode::save_snap(const proto::Snapshot& snap) {
  // must save the snapshot index to the WAL before saving the
  // snapshot to maintain the invariant that we only Open the
  // wal at previously-saved snapshot indexes.
  // 必须在保存快照之前就把快照的index（以及序列化的snapshot）作为日志条目保存到WAL FILE中
  // 以确保我们维持了这个准备：我们只打开之前保存过snapshot index的wal
  Status status;

  WAL_Snapshot wal_snapshot;
  wal_snapshot.index = snap.metadata.index;
  wal_snapshot.term = snap.metadata.term;
// 先存到WAL FILE 里
  status = wal_->save_snapshot(wal_snapshot);
  if (!status.is_ok()) {
    return status;
  }

  status = snapshotter_->save_snap(snap);
  if (!status.is_ok()) {
    LOG_FATAL("save snapshot error %s", status.to_string().c_str());
  }
  return status;

  return wal_->release_to(snap.metadata.index);
}

void RaftNode::publish_snapshot(const proto::Snapshot& snap) {
  if (snap.is_empty()) {
    return;
  }
// 将当前已应用的最大日志索引记录，方便debug
  LOG_DEBUG("publishing snapshot at index %lu", snapshot_index_);
// applied index代表了已经应用到状态机上的最新索引。如果快照的索引小于等于它，代表了系统在某个更早
// 时候的状态。如果还应用这个快照会导致系统回滚 或者数据不一样。在应用这样的快照事，可能回重新应用
// 已经在系统中应用过的日志条目，导致重复操作，甚至数据错误。
  if (snap.metadata.index <= applied_index_) {
    LOG_FATAL("snapshot index [%lu] should > progress.appliedIndex [%lu] + 1", snap.metadata.index, applied_index_);
  }

  //trigger to load snapshot 到SnapshotPtr和SnapshotDataPtr这两个指针指向的内存中。都是从传入Snap中来
  proto::SnapshotPtr snapshot(new proto::Snapshot());
  snapshot->metadata = snap.metadata;
  SnapshotDataPtr data(new std::vector<uint8_t>(snap.data));
// 更新系统的conf state（节点的配置信息），snapshot和applied index
  *(this->conf_state_) = snapshot->metadata.conf_state;
  snapshot_index_ = snapshot->metadata.index;
  applied_index_ = snapshot->metadata.index;
// 通过Redis服务器从快照中恢复数据。recover函数会从snapshot中恢复数据。包括插入缺失数据，更新现有数据，删除多余数据达尔博格
// data是在前面建立的SnapshotDataPtr。如果是应用snapshot到WAL。应该主要是插入没有的index对应的日志条目
  redis_server_->recover_from_snapshot(data, [snapshot, this](const Status& status) {
    //由redis线程回调
    if (!status.is_ok()) {
      LOG_FATAL("recover from snapshot error %s", status.to_string().c_str());
    }

    LOG_DEBUG("finished publishing snapshot at index %lu", snapshot_index_);
  });

}
/** 打开给定snap对应的时间点的WAL */
void RaftNode::open_WAL(const proto::Snapshot& snap) {
  if (!boost::filesystem::exists(wal_dir_)) {
    boost::filesystem::create_directories(wal_dir_);
    WAL::create(wal_dir_);
  }

  WAL_Snapshot walsnap;
  walsnap.index = snap.metadata.index;
  walsnap.term = snap.metadata.term;
  LOG_INFO("loading WAL at term %lu and index %lu", walsnap.term, walsnap.index);

  wal_ = WAL::open(wal_dir_, walsnap);
}

/** 刚刚的open_WAL 是这个函数的一个子集 */
void RaftNode::replay_WAL() {
  LOG_DEBUG("replaying WAL of member %lu", id_); // 记录调试信息，表示正在回放指定的WAL日志

  proto::Snapshot snapshot;
  Status status = snapshotter_->load(snapshot);
  if (!status.is_ok()) { 
    if (status.is_not_found()) {
      LOG_INFO("snapshot not found for node %lu", id_);
    } else {
      LOG_FATAL("error loading snapshot %s", status.to_string().c_str());
    }
  } else {
    storage_->apply_snapshot(snapshot); // 将snapshot的状态加载到memory中。作为日志状态的初始化
  }
// 注意这个函数之后wal_变量已经变为从该snapshot开始的状态了
  open_WAL(snapshot); // 打开snapshot对应的WAL。从该snapshot往后的所有WAL FILE都会被夹在。
  assert(wal_ != nullptr);

  proto::HardState hs;
  std::vector<proto::EntryPtr> ents;
  status = wal_->read_all(hs, ents); // 回放从snapshot开始所有日志。并更新hs和ents的数据
  if (!status.is_ok()) {
    LOG_FATAL("failed to read WAL %s", status.to_string().c_str());
  }

  storage_->set_hard_state(hs);

  // append to storage so raft starts at the right place in log
  storage_->append(ents);

  // send nil once lastIndex is published so client knows commit channel is current
  if (!ents.empty()) {
    last_index_ = ents.back()->index;
  } else {
    snap_data_ = std::move(snapshot.data);
  }
}

/** 该函数用于处理并应用一系列的日志条目，这些entries可能是普通或者配置更改条目。
 * 它通过迭代每个entry，根据其类型进行不同的处理，并在处理完成后更新已应用的日志索引？
 * 问题：交给Redis-server处理的那部分是什么？更新数据库吗？
 */
bool RaftNode::publish_entries(const std::vector<proto::EntryPtr>& entries) {
  for (const proto::EntryPtr& entry : entries) {
    switch (entry->type) {
      case proto::EntryNormal: {
        if (entry->data.empty()) {
          // ignore empty messages
          break;
        }
        redis_server_->read_commit(entry); // 数据不为空，将日志条目提交给redis_server_ 处理
        break;
      }

      case proto::EntryConfChange: {
        proto::ConfChange cc;
        try {
          // 将数据解包为ConfChange对象（node配置）
          msgpack::object_handle oh = msgpack::unpack((const char*) entry->data.data(), entry->data.size());
          oh.get().convert(cc);
        }
        catch (std::exception& e) {
          LOG_ERROR("invalid EntryConfChange msg %s", e.what());
          continue;
        }
        conf_state_ = node_->apply_conf_change(cc); // 应用配置更改
// 如果是添加节点，则调用add-pear添加新节点。如果是移除节点，且被移除的是当前节点，
// 表示当前节点可能被移除。给出warning消息，并不做操作，返回false。否则调用remove peer，移除指定节点
// 其他就是正常的状态变更。显示msg即可
        switch (cc.conf_change_type) { 
          case proto::ConfChangeAddNode: 
            if (!cc.context.empty()) {
              std::string str((const char*) cc.context.data(), cc.context.size());
              transport_->add_peer(cc.node_id, str);
            }
            break;
          case proto::ConfChangeRemoveNode:
            if (cc.node_id == id_) {
              LOG_INFO("I've been removed from the cluster! Shutting down.");
              return false;
            }
            transport_->remove_peer(cc.node_id);
          default: {
            LOG_INFO("configure change %d", cc.conf_change_type);
          }
        }
        break;
      }
      default: {
        LOG_FATAL("unknown type %d", entry->type);
        return false;
      }
    }

    // after commit, update appliedIndex。提交该日志后，将应用的index更新为当前提交的index
    applied_index_ = entry->index;

    // replay has finished
    if (entry->index == this->last_index_) {
      LOG_DEBUG("replay has finished");
    }
  }
  return true;
}

/** 这个函数将需要应用的条目从输入的entris中筛选出来（因为有些可能已经在ents中存在，插入到ents中） */
void RaftNode::entries_to_apply(const std::vector<proto::EntryPtr>& entries, std::vector<proto::EntryPtr>& ents) {
  if (entries.empty()) {
    return;
  }

  uint64_t first = entries[0]->index;
  // 如果entires中的第0个日志条目都比目前应用的最后一个日志条目大1以上，插入会导致日志条目不连续
  // 注意entries中0之后的日志 index只会更大。
  if (first > applied_index_ + 1) {
    LOG_FATAL("first index of committed entry[%lu] should <= progress.appliedIndex[%lu]+1", first, applied_index_);
  }
  // 如果entries 中有可以插入ents的条目。注意只插入applied index后面的
  if (applied_index_ - first + 1 < entries.size()) {
    ents.insert(ents.end(), entries.begin() + applied_index_ - first + 1, entries.end());
  }
}
/** 根据一定条件出发快照操作，当应用的日志条目数量超过设定的snap-count，创建新快照，压缩日志条目 */
void RaftNode::maybe_trigger_snapshot() {
  if (applied_index_ - snapshot_index_ <= snap_count_) { // 检查是否需要触发快照
    return;
  }

  LOG_DEBUG("start snapshot [applied index: %lu | last snapshot index: %lu], snapshot count[%lu]",
            applied_index_,
            snapshot_index_,
            snap_count_);
// promise 和future用于异步获取快照数据
  std::promise<SnapshotDataPtr> promise;
  std::future<SnapshotDataPtr> future = promise.get_future();
/**
 * redis_server_->get_snapshot 是获取当前redis-server上的最新快照数据。因为涉及到i/o或其他耗时操作，因此
 * 使用异步方法，避免阻塞住thread。snapshot获取完成后。通过promise.set)_value 将数据传回主线程
 * 通过从 Redis 服务器获取快照，可以确保所有节点获取到的是一致的快照数据，
 * 避免因节点本地生成快照时的延迟导致的数据不一致问题。
 */
  redis_server_->get_snapshot(std::move([&promise](const SnapshotDataPtr& data) {  // 接入rocksdb 注意这里拿到的是kv pair的vector 序列化数据
    promise.set_value(data);
  }));

  future.wait(); // 等待快照数据的获取完成，并通过future.get()获取快照数据
  SnapshotDataPtr snapshot_data = future.get();

  proto::SnapshotPtr snap; // 在内存中创建新的快照，根据现在的状态.查看一下是否有kv store。 Snapshot中的data是vector<uint_8>.rocksdb应该也可以直接存储
  Status status = storage_->create_snapshot(applied_index_, conf_state_, *snapshot_data, snap);
  if (!status.is_ok()) {
    LOG_FATAL("create snapshot error %s", status.to_string().c_str());
  }

  status = save_snap(*snap); // 保存创建的快照到目前的系统里
  if (!status.is_ok()) {
    LOG_FATAL("save snapshot error %s", status.to_string().c_str());
  }
/** defaultSnapCount控制我们多少个log产生一个compact snapshot。比如我们现在设为4，那么每四个产生一个snapshot
 * snapshotCatchUpEntriesN 控制我们删除前多少个log。两个都通过 maybe_trigger_snapshot() 
 * 比如我们将snapshotCatchUpEntriesN设为2. 那么当现在的applied_index_ - (last) snapshot_index_ > 4
 * 则会产生一个snapshot。也就是当第五个entry（applied_index_）发生时，前四个产生snapshot。
 * 同时 compactIndx = 5 - 2 = 3,
 * 也就是说Index = 3 以前的日志会被删除（3自己不会删掉，同时3成为arr第0位）。这就是compact
 */
// 压缩日志条目。这里的逻辑就是每次就删除snapshot cnt规定的那么多的最前面的条目。compactIdx是绝对值
// applied index = 1100, snapshotCatchUpEntiresN = 1000. compactIdx = 100. delete 0-100 entries
// applied index = 1200, snapshotCatchUpentriesN = 1000, compactIdx = 200. delete 0-200 entreis. 
  uint64_t compactIndex = 1;
  if (applied_index_ > snapshotCatchUpEntriesN) {
    compactIndex = applied_index_ - snapshotCatchUpEntriesN;
  }
  status = storage_->compact(compactIndex);
  if (!status.is_ok()) {
    LOG_FATAL("compact error %s", status.to_string().c_str());
  }
  LOG_INFO("compacted log at index %lu", compactIndex);
  snapshot_index_ = applied_index_;
}

/** 该函数用于初始化Raft 节点的调度，包括获取快照数据，初始化Redis 服务器以及启动定时器和I/O服务 */
void RaftNode::schedule() {
  pthread_id_ = pthread_self();

  proto::SnapshotPtr snap;
  // 为什么在初始化Raft节点前，就会在内存中有snapshot？不是应该初始化Raft之后才有内存吗？
  /**
   * 如果 storage_ 是 MemoryStoragePtr 类型，这确实表示存储在内存中的日志和快照数据。
   * 对于一个刚刚初始化的 Raft 节点，我们需要考虑如何初始化这个内存存储以及它为什么会有快照数据。
   * 通常，这意味着在系统启动时，必须有一个过程将持久化存储的数据加载到内存中。
内存存储的初始化
在实际的系统中，即使内存存储类（如 MemoryStorage）被用来处理当前运行时的数据，
仍然需要从持久化存储中加载这些数据到内存中。这通常是在系统启动或节点初始化时完成的。
可能的流程
1. 系统启动：系统从持久化存储（如磁盘、数据库等）中读取数据，并将其加载到内存存储中。
2. 内存存储加载数据：MemoryStorage 被初始化时，从持久化存储加载快照和日志数据。
3. Raft 节点初始化：RaftNode 在初始化过程中，从 MemoryStorage 中获取当前快照数据。
   */
  Status status = storage_->snapshot(snap); // 获取目前内存中的snapshot到变量snap中。注意在
  if (!status.is_ok()) {
    LOG_FATAL("get snapshot failed %s", status.to_string().c_str());
  }
// 更新配置状态和索引
  *conf_state_ = snap->metadata.conf_state; 
  snapshot_index_ = snap->metadata.index;
  applied_index_ = snap->metadata.index;
// 使用快照数据和端口号初始化Redis 服务器。所以这里snap_data_,port_应该也提前初始化了。
// RedisStore 是一个类，表示一个 Redis 服务器实例。这个类的构造函数需要三个参数：RaftNode 的指针、
// 快照数据、以及端口号。
// this:RaftNode ptr, 指向当前RaftNode。move：将snap data所有权转移给RedisStore的构造函数，避免
// 不必要的拷贝。一旦完成转移，RaftNode中的快照将变为空。（注意不是整个RaftNode变为空，只将SnapData 
// 转移给Redis Server，因为它的存储和访问效率更高）
// port_是一个整数。表示Redis 服务器将监听的端口号

/** Lu:将快照数据传递给 RedisStore 的 snap_data_*/
  snap_data_ = std::move(snap->data);

  redis_server_ = std::make_shared<RedisStore>(this, std::move(snap_data_), port_, id_);
  // 启动Redis 服务器
  std::promise<pthread_t> promise; // 启动promise和future对象，用于异步启动Redis服务器
  std::future<pthread_t> future = promise.get_future();
  redis_server_->start(promise);
  future.wait(); // 阻塞当前线程，直到Redis服务器启动完成
  pthread_t id = future.get(); // 获取Redis服务器启动的线程id。
  LOG_DEBUG("server start [%lu]", id);

  start_timer();
  io_service_.run();
}
/** Raft节点处提案。data指向需要被提议的数据。callback用于处理提案的结果 
 * 代码的作用
线程安全性：确保在正确的线程上下文中处理提案。主线程可以直接处理提案，而其他线程将提案提交到 io_service_ 进行异步处理。
提案处理：通过 node_->propose 提交提案，并返回提案状态。
回调通知：通过回调函数通知调用者提案的处理结果。
事件处理：在提案处理完成后，调用 pull_ready_events 处理任何准备好的事件。
*/
void RaftNode::propose(std::shared_ptr<std::vector<uint8_t>> data, const StatusCallback& callback) {
  //  该比较确定当前thread是否在主线程运行
  if (pthread_id_ != pthread_self()) { // pthread_id_ is the main thread id
    io_service_.post([this, data, callback]() { // 如果当前线程不是主线程，利用io_service_提交
    // 一个异步任务。注意是不是主线程的区别主要是需不需要通过io service提交一个异步任务
      Status status = node_->propose(std::move(*data)); // 将提议转移给当前node->propose
      callback(status); // 执行回调函数，传递提案状态。
      pull_ready_events(); // 处理任何准备好的事件
    });
  } else {
    Status status = node_->propose(std::move(*data));
    callback(status);
    pull_ready_events();
  }
}
/**
 * 该函数与propose类似，区别在于process处理传入的消息。
 */
void RaftNode::process(proto::MessagePtr msg, const StatusCallback& callback) {
  if (pthread_id_ != pthread_self()) {
    io_service_.post([this, msg, callback]() {
      Status status = this->node_->step(msg);
      callback(status);
      pull_ready_events();
    });
  } else {
    Status status = this->node_->step(msg);
    callback(status);
    pull_ready_events();
  }
}

void RaftNode::is_id_removed(uint64_t id, const std::function<void(bool)>& callback) {
  LOG_DEBUG("no impl yet");
  callback(false);
}

void RaftNode::report_unreachable(uint64_t id) {
  LOG_DEBUG("no impl yet");
}

void RaftNode::report_snapshot(uint64_t id, SnapshotStatus status) {
  LOG_DEBUG("no impl yet");
}

static RaftNodePtr g_node = nullptr;

void on_signal(int) {
  LOG_INFO("catch signal");
  if (g_node) {
    g_node->stop();
  }
}

void RaftNode::main(uint64_t id, const std::string& cluster, uint16_t port) {
  // 注册新号处理函数， SIGINT， SIGHUP。这些信号通常用于处理程序终止或重新加载配置的请求
  ::signal(SIGINT, on_signal);
  ::signal(SIGHUP, on_signal);
  g_node = std::make_shared<RaftNode>(id, cluster, port); // 创建RaftNode实例，并付给全局变量
// 初始化传输层，并启动传输。 Transport::create用于创建传输层实例。获取当前节点的主机地址host，并启动传输层，使其监听该地址
  g_node->transport_ = Transport::create(g_node.get(), g_node->id_);
  std::string& host = g_node->peers_[id - 1]; // id -1就是该节点在peers中的位置。因为节点id从1开始1
  g_node->transport_->start(host);
// 添加集群中的其他节点到传输层
  for (uint64_t i = 0; i < g_node->peers_.size(); ++i) {
    uint64_t peer = i + 1;
    if (peer == g_node->id_) {
      continue;
    }
    g_node->transport_->add_peer(peer, g_node->peers_[i]);
  }
// 调用schedule启动Raft的调度逻辑。调度过程中的获取快照数据是为了确保节点能够从持久化存储中恢复其
// 先前的状态。在这里应该是恢复cluster同步过的状态。
  g_node->schedule();
}

void RaftNode::stop() {
  LOG_DEBUG("stopping");
  redis_server_->stop();

  if (transport_) {
    transport_->stop();
    transport_ = nullptr;
  }
  io_service_.stop();
}

}
/**
 * 在 Raft 分布式一致性算法的实现中，RaftNode 文件通常用于表示集群中的一个节点，并管理该节点的状态、角色和与其他节点的通信。RaftNode 是 Raft 协议实现中的一个核心组件，它负责处理 Raft 协议的逻辑，包括领导选举、日志复制和一致性保证。

在 RaftNode 中，常见的字段包括：

id:

这是节点的唯一标识符，用于在集群中区分不同的节点。每个节点在启动时都会分配一个唯一的 ID。
cluster:

这个字段通常表示该节点所属的集群信息，包含了集群中所有节点的信息。通过 cluster，一个节点可以知道其他节点的地址、端口等信息，以便进行通信和同步。
port:

这是该节点用于通信的网络端口。每个 RaftNode 都在指定的端口上监听和处理来自其他节点的请求。
具体功能
节点状态管理: RaftNode 会维护节点的当前状态（如 follower、candidate、leader）和日志信息。
领导选举: 当集群中的领导节点失效时，RaftNode 会参与选举过程，尝试成为新的领导者。
日志复制: 领导节点负责将客户端的命令写入日志，并将这些日志复制到其他节点上，确保所有节点的数据一致性。
心跳机制: 领导节点会定期向其他节点发送心跳消息，保持其领导地位，并检测节点的存活状态。
故障恢复: 当某些节点失败或网络分区恢复后，RaftNode 会协调状态和日志的恢复，以确保集群的一致性和可用性。
 */
