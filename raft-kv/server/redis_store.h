#pragma once
#include <boost/asio.hpp>
#include <unordered_map> // we will change this to rocksdb
#include <rocksdb/db.h>  //Todo: we need to add path of rocksdb to CMakelist file later 
#include <thread>
#include <future>
#include <raft-kv/common/status.h>
#include <raft-kv/raft/proto.h>
#include <msgpack.hpp>
/** 这段代码定义了名为RedisStore的类，用于实现分布式系统中的数据存储和处理逻辑,其中Redis被用作底层
 * 的数据存储(是缓存形式的存储，因此write-read速度极快)。
 * 我们在这里将它替换成rocksdb，永久性存储？
 */
namespace kv {
/** 用于字符串匹配 */
int string_match_len(const char* pattern, int patternLen,
                     const char* string, int stringLen, int nocase);
/**RaftCommitData是 结构体，包含了Redis提交操作的数据。
 * type表示提交操作的类型，可能是kCommitSet（设置数据）或
 * kCommitDel（删除数据）。
 * strs时一个字符串响亮，存储与提交操作相关的字符串数据
 * MSGPACK_DEFINE 是一个宏，用于定义这个结构体如何通过MessagePack进行序列化和反序列化
 */
struct RedisCommitData {
  static const uint8_t kCommitSet = 0;
  static const uint8_t kCommitDel = 1;

  uint8_t type;
  std::vector<std::string> strs;
  MSGPACK_DEFINE (type, strs);
};
/** RaftCommit是结构体，表示一个Raft协议中的提交操作。
 * node id是执行提交操作的节点ID
 * commit id时提交操作的唯一标识符
 * redis data 包含与该提交相关的RedisCOmmitData（上面的struc定义了）
 */
struct RaftCommit {

  RaftCommit() {}

  uint32_t node_id;
  uint32_t commit_id;
  RedisCommitData redis_data;
  MSGPACK_DEFINE (node_id, commit_id, redis_data); // 定义了需要序列化哪些内容
};
// 注意function<>是模版，表示接受一个function，返回void，接受Status作为参数。用于回调状态信息
typedef std::function<void(const Status&)> StatusCallback;
typedef std::shared_ptr<std::vector<uint8_t>> SnapshotDataPtr;  // 用于处理快照数据
typedef std::function<void(SnapshotDataPtr)> GetSnapshotCallback; // 用于处理从系统中获取的快照数据

class RaftNode;
class RedisStore {
 public:
  explicit RedisStore(RaftNode* server, std::vector<uint8_t> snap, uint16_t port, uint64_t id);

  ~RedisStore();
/** 停止I/O服务并等待工作线程结束 */
  void stop() {
    io_service_.stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }
/** 启动服务，并通过promise返回线程ID */
  void start(std::promise<pthread_t>& promise);
/* 通过关键字查询值 */
  bool get(const std::string& key, std::string& value) {
    auto it = key_values_.find(key);
    if (it != key_values_.end()) {
      value = it->second;
      return true;
    } else {
      return false;
    }
  }
/** 设置kv pair */
  void set(std::string key, std::string value, const StatusCallback& callback);
/** 删除kv pair */
  void del(std::vector<std::string> keys, const StatusCallback& callback);
/** 获取系统的快照数据，并通过回调函数返回 */
  void get_snapshot(const GetSnapshotCallback& callback);
/** 从快照数据恢复系统状态， 并调用回调函数返回操作状态 */
  void recover_from_snapshot(SnapshotDataPtr snap, const StatusCallback& callback);
/** 根据模式匹配返回所有符合条件的key */
  void keys(const char* pattern, int len, std::vector<std::string>& keys);
/** 处理提交的日志条目 */
  void read_commit(proto::EntryPtr entry);

/** for rocksdb snapshot generate*/
  void createRocksDBCheckpoint();

 private:
  void start_accept(); // 开始接受client连接

  RaftNode* server_;  // 指向RaftNode实例的指针，表示当前节点。在这里与Raft Node相连。我们可以获取
  // rocksdb的指针和存储位置。
  boost::asio::io_service io_service_; // Boost.Asio的I/O服务对象，用于处理异步I/O操作
  boost::asio::ip::tcp::acceptor acceptor_; // TCP接受器，用于接受网络连接
  std::thread worker_;  // 工作线程，用于处理I/O事件
  std::unordered_map<std::string, std::string> key_values_;  // 存储kv pair的哈希表。我们将它替换成rocksdb database

  uint32_t next_request_id_;  // 下一个请求的ID
  std::unordered_map<uint32_t, StatusCallback> pending_requests_;  // 存储挂起请求的回调函数

  std::string rocksdb_dir_;
  rocksdb::DB* db_;
};

}
/**
 * boost::asio::ip::tcp::acceptor 是 Boost.Asio 库的一部分，用于接受传入的 TCP 连接。
 * 它是一个面向对象的类，封装了创建、绑定和监听 TCP 连接的底层操作。
主要功能
绑定到端口：acceptor 可以绑定到指定的端口，监听来自客户端的连接请求。
接受连接：它可以异步或同步地接受传入的连接请求。
管理连接：可以设置选项和属性，如重用地址选项、超时设置等。
 */
