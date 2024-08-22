#include <msgpack.hpp>
#include <raft-kv/server/redis_store.h>
#include <raft-kv/server/raft_node.h>
#include <raft-kv/common/log.h>
#include <raft-kv/server/redis_session.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <rocksdb/utilities/checkpoint.h>  // for rocksdb snapshot the db in binary file

#include <rocksdb/iterator.h>  // for rocksdb
#include <rocksdb/write_batch.h> // for rocksdb

/** 
 * RaftNode 中的redis_server_实际上就是指向Redis_store的指针。因此这个类应该就是负责接收Raft Commit的
 * 数据以及与Redis数据库的交互
 */
namespace kv {

// see redis keys command。这个函数是一个用于匹配模式字符串pattern和目标字符串string的函数
// 它支持通配符匹配，包括`*`，`？`, `[]`,以及大小写敏感和不敏感陪陪。
int string_match_len(const char* pattern, int patternLen,
                     const char* string, int stringLen, int nocase) {
  while (patternLen && stringLen) { // 当模式字符串和目标字符串都不为空，进入循环
    switch (pattern[0]) {
      case '*':
        while (pattern[1] == '*') { //连续的*只保留一个。*可以匹配0到无限个
          pattern++;
          patternLen--;
        }
        if (patternLen == 1) // 此时*是模式字符串的最后一个字符，可以匹配无限个string中的字符。一定匹配
          return 1; /* match */
        while (stringLen) { //否则，递归匹配目标字符串中的每一个位置，直到找到匹配或者目标字符串为空
          if (string_match_len(pattern + 1, patternLen - 1,
                               string, stringLen, nocase))
            return 1; /* match */
          // 这里的逻辑是 虽然我们刚才把所有的*都忽略了。但实际上我们可以通过string++让*匹配一个字母。
          string++;
          stringLen--;
        }
        return 0; /* no match */ // 走到string的尽头了都没办法匹配
        break;
      case '?':
      // ‘？’ 匹配任意单个字符，匹配成功后移动到下一个字符（类似.）
        if (stringLen == 0)
          return 0; /* no match */
        string++;
        stringLen--;
        break;
      case '[': { 
      // 处理字符集，包括反向字符集[^...] 和范围字符（‘a-z’）
        int not_match, match;

        pattern++;
        patternLen--;
        not_match = pattern[0] == '^'; // 如果【后的第一个字符是^ 说明需要反向匹配
        if (not_match) {
          pattern++;
          patternLen--;
        }
        match = 0;
        while (1) { // 处理字符集中每一个字符或范围，直到遇到右括号
          if (pattern[0] == '\\' && patternLen >= 2) { //注意 '\\'是一个char，第一个\表示第二个\真的是
            pattern++;
            patternLen--;
            if (pattern[0] == string[0])
              match = 1;
          } else if (pattern[0] == ']') {
            break;
          } else if (patternLen == 0) { // pattern已经到头但没遇到】，恢复pattern到【之前模式。
            pattern--;
            patternLen++;
            break;
          } else if (pattern[1] == '-' && patternLen >= 3) { // 【a-z】pattern
            int start = pattern[0];
            int end = pattern[2];
            int c = string[0];
            if (start > end) { // 如果字符范围起始字符大雨结束字符，交换它们的顺序
              int t = start;
              start = end;
              end = t;
            }
            if (nocase) {
              start = tolower(start);
              end = tolower(end);
              c = tolower(c);
            }
            pattern += 2; // 跳过字符范围的三个字符
            patternLen -= 2;
            // 字符s【0】匹配上
            if (c >= start && c <= end)
              match = 1;
          } else {
            if (!nocase) {
              if (pattern[0] == string[0])
                match = 1;
            } else {
              if (tolower((int) pattern[0]) == tolower((int) string[0]))
                match = 1;
            }
          }
          pattern++;
          patternLen--;
        }
        // 否定更新匹配。match=1为匹配上
        if (not_match)
          match = !match;
        if (!match)
          return 0; /* no match */
        string++;
        stringLen--;
        break;
      }
      case '\\':
        if (patternLen >= 2) { // 处理转义字符'\'，将下一个字符视为普通字符
          pattern++;
          patternLen--;
        }
        /* fall through */
      default:
        if (!nocase) {
          if (pattern[0] != string[0])
            return 0; /* no match */
        } else {
          if (tolower((int) pattern[0]) != tolower((int) string[0]))
            return 0; /* no match */
        }
        string++; // 还要进入下一个循环进行匹配
        stringLen--;
        break;
    }
    pattern++; // 还要进入下一个循环
    patternLen--;
    if (stringLen == 0) {
      while (*pattern == '*') {
        pattern++;
        patternLen--;
      }
      break;
    }
  }
  if (patternLen == 0 && stringLen == 0)
    return 1;
  return 0;
}
/** constructor从snapshot中恢复KV store 的数据。注意Redis kv store主要通过快照来恢复数据存储
 * 所有的数据都会存在快照中。导致快照会变的很大，所以需要定期compact和清理
 * 同时用boost::asio::acceptor开始监听指定的port */
RedisStore::RedisStore(RaftNode* server, std::vector<uint8_t> snap, uint16_t port, uint64_t id)
    : server_(server),
      acceptor_(io_service_),
      next_request_id_(0) {

  std::string work_dir = "node_" + std::to_string(id);
  /** Rocksdb: create rocksdb db based on node id, noting work_dir already node specific */
  rocksdb_dir_ = work_dir + "/db";
  rocksdb::Options options;
  options.create_if_missing = true;
  /** create this path not exist */
  if (!boost::filesystem::exists(rocksdb_dir_)) {
    boost::filesystem::create_directories(rocksdb_dir_);
  }

  rocksdb::Status st = rocksdb::DB::Open(options, rocksdb_dir_, &db_);
  if (!st.ok()) { // 这里就已经读到了自己的database，即使重启。
    throw std::runtime_error("Failed to open RocksDB: " + st.ToString());
  }

  // if (!snap.empty()) { // 如果传进来的snap不为空. for rocksdb we need to modify here !
  //   std::unordered_map<std::string, std::string> kv;
  //   msgpack::object_handle oh = msgpack::unpack((const char*) snap.data(), snap.size());

  //   try {
  //     oh.get().convert(kv);
  //   } catch (std::exception& e) {
  //     LOG_WARN("invalid snapshot: %s", e.what()); // for test
  //   }
  //   std::swap(kv, key_values_); // 用kv mp 值replace key_value_。snap代表最新的状态
  // }

  /** for rocksdb initialize if using passed snap */
  if (!snap.empty()) { // 如果传进来的snap不为空. for rocksdb we need to modify here !
    std::vector<std::pair<std::string, std::string>> key_values;
    msgpack::object_handle oh = msgpack::unpack((const char*) snap.data(), snap.size());

    try {
      oh.get().convert(key_values);
    } catch (std::exception& e) {
      LOG_WARN("invalid snapshot: %s", e.what()); // for test
    }
    load_kv_to_rocksdb(key_values);  // 注意这里我们已经load rocksdb::DB* db_了
  }

  auto address = boost::asio::ip::address::from_string("0.0.0.0"); // 这个地址对吗？
  auto endpoint = boost::asio::ip::tcp::endpoint(address, port); // port是传入的值

  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(1));
  acceptor_.bind(endpoint);
  acceptor_.listen();
}

RedisStore::~RedisStore() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void RedisStore::start(std::promise<pthread_t>& promise) {
  start_accept();
// worker 是当前线程
  worker_ = std::thread([this, &promise]() {
    promise.set_value(pthread_self());
    this->io_service_.run();
  });
}

void RedisStore::start_accept() {
  RedisSessionPtr session(new RedisSession(this, io_service_));
// acceptor是用来监听传入的连接。 async是可以异步处理，不会阻塞后面新的start-accept。
// session->socket_ 代表新链接的客户端套接字
  acceptor_.async_accept(session->socket_, [this, session](const boost::system::error_code& error) {
    if (error) {
      LOG_DEBUG("accept error %s", error.message().c_str());
      return;
    }
    this->start_accept(); // 递归调用自身来开始另一次以不接受操作。为了继续监听新的传入链接。从而实现服务器能够持续接受新的client connection
    session->start();  // 启动新回话，会话通常是处理客户端 服务器之间通信的核心部分。
  });
}

/** 该方法用于将一个kv pair提交给Raft cluster进行共识决策。 */
void RedisStore::set(std::string key, std::string value, const StatusCallback& callback) {
  uint32_t commit_id = next_request_id_++; // 生成唯一的请求ID，比目前最新的commitid 大1
// 创建Raft提交对象
  RaftCommit commit;
  commit.node_id = static_cast<uint32_t>(server_->node_id());
  commit.commit_id = commit_id;
  commit.redis_data.type = RedisCommitData::kCommitSet;
  commit.redis_data.strs.push_back(std::move(key)); // 将kv传入commit的data 中
  commit.redis_data.strs.push_back(std::move(value));

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, commit);
  std::shared_ptr<std::vector<uint8_t>> data(new std::vector<uint8_t>(sbuf.data(), sbuf.data() + sbuf.size()));
// 将回调函数与commid id关联。并存储在pending-request 中。这些回调函数将在请求完成时被调用。
  pending_requests_[commit_id] = callback; // map: commid_id -> StatusCallBack
// 将请求提交给Raft cluster： 调用server->propose 提交数据给Raft cluster。
// 提交成功后，回调用提供的lambda回调函数。如果提交成功，直接返回。
// 如果操作失败，从pending requests 中找到对应的回调函数并调用。然后从pending中删除该变量。
  server_->propose(std::move(data), [this, commit_id](const Status& status) {
    io_service_.post([this, status, commit_id]() {
      if (status.is_ok()) {
        return;
      }

      auto it = pending_requests_.find(commit_id);
      if (it != pending_requests_.end()) {
        it->second(status); // status is not OK。status作为参数传入，有相应的处理
        pending_requests_.erase(it);
      }
    });
  });
}
/** 提交一个删除key的操作给 Raft cluster，进行共识决策 */
void RedisStore::del(std::vector<std::string> keys, const StatusCallback& callback) {
  uint32_t commit_id = next_request_id_++;

  RaftCommit commit;
  commit.node_id = static_cast<uint32_t>(server_->node_id());
  commit.commit_id = commit_id;
  commit.redis_data.type = RedisCommitData::kCommitDel; // 这里indicate 了是del
  commit.redis_data.strs = std::move(keys);
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, commit);
  std::shared_ptr<std::vector<uint8_t>> data(new std::vector<uint8_t>(sbuf.data(), sbuf.data() + sbuf.size()));

  pending_requests_[commit_id] = callback;
// 为什么这里不需要检查status就可以直接删除pending request（注意不是删除该key）
// 在set函数中，commit成功时，回调函数可能会被其他机制处理。所以不再重复调用回调函数。
  server_->propose(std::move(data), [this, commit_id](const Status& status) {
    io_service_.post([commit_id, status, this]() {
// 无论成功还是失败，都需要从pending request中找到该请求并回调和删除。
      auto it = pending_requests_.find(commit_id);
      if (it != pending_requests_.end()) {
        it->second(status);
        pending_requests_.erase(it);
      }
    });
  });
}

/** for generate rocksdb checkpoint */
void RedisStore::createRocksDBCheckpoint() {
  rocksdb::Checkpoint* checkpoint;

  rocksdb::Status status = rocksdb::Checkpoint::Create(db_, &checkpoint);
  if (!status.ok()) {
    LOG_ERROR("Cannot open the database.");
    return;
  }
  std::string checkpoint_dir = rocksdb_dir_ + std::to_string(std::time(nullptr));
  status = checkpoint->CreateCheckpoint(checkpoint_dir);
  delete checkpoint;
  if (!status.ok()) {
    LOG_ERROR("Cannot create snapshot of rocksdb.");
    return;
  }
}

/** read all key-value pairs from current rocksdb to key_values */
void RedisStore::get_all_key_value(std::vector<std::pair<std::string, std::string>>& key_values) {
  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_options));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    std::string value = it->value().ToString();
    key_values.push_back({key, value});
  }

  if (!it->status().ok()) {
    LOG_ERROR("An error occurred during iteration: %s", it->status().ToString());
  }
}

// void RedisStore::get_snapshot(const GetSnapshotCallback& callback) {
//   io_service_.post([this, callback] {
//     msgpack::sbuffer sbuf;
//     // for rocksdbm we use vector<>
//     std::vector<std::pair<std::string, std::string>> key_values;
//     msgpack::pack(sbuf, key_values_);
//     // msgpack::pack(sbuf, key_values);  // modify here!
//     /**
//      * SnapshotDataPtr 是一个类型定义，表示一个 std::shared_ptr，指向 std::vector<uint8_t> 类型的对象。
// std::vector<uint8_t> 是一个动态数组，专门用于存储字节数据（uint8_t 是表示 8 位无符号整数的类型，
// 通常用于处理二进制数据）。
// (sbuf.data(), sbuf.data() + sbuf.size())：这个构造函数接受两个指针，表示一个字节范围。
// sbuf.data() 是起始指针，指向 sbuf 中数据的起始位置，
// sbuf.data() + sbuf.size() 是结束指针，指向 sbuf 中数据的结束位置。
//      */
//     SnapshotDataPtr data(new std::vector<uint8_t>(sbuf.data(), sbuf.data() + sbuf.size()));
//     callback(data);
//   });
// }

/** 根据现在的key value map创建新的snapshot - rocksdb version. replace kv store with db_ */
void RedisStore::get_snapshot(const GetSnapshotCallback& callback) { // raft_node 里回调了这个函数。获得了data的数据。但是不需要读取到任何新的kv store中。
// 所以不需要修改
  io_service_.post([this, callback] {
    msgpack::sbuffer sbuf;
    // for rocksdbm we use vector<>
    std::vector<std::pair<std::string, std::string>> key_values;
    get_all_key_value(key_values); // add here!
    msgpack::pack(sbuf, key_values);
    SnapshotDataPtr data(new std::vector<uint8_t>(sbuf.data(), sbuf.data() + sbuf.size()));
    callback(data); // 这个回调给哪里-RaftNode。cpp
  });
}

// using write_batch to read kv data from msgpack serialized data and merge into local database (include write, delete, udpate kv)
void RedisStore::load_kv_to_rocksdb(const std::vector<std::pair<std::string, std::string>>& key_values) {

  // delete operation is not included in batch operation. we need to delete by ourselves
  std::vector<std::string> keys_to_delete;
  rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
  // iterate the current kv store, if any key is not in key_values, delete it
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    bool found = false;
    for (const auto& kv : key_values) {
      if (kv.first == key) {
        
        found = true;  // found, not need to delete
        break;
      }
      if (!found) {
        keys_to_delete.push_back(key);
      }
    }
  }
  delete it;

  rocksdb::WriteBatch batch; // we will put all we need to delete and insert in batch, and pass to the batchWriter
  // We lable the data as Delete or Put. Put is for udpate or insert
  for (const std::string& key : keys_to_delete) {
    batch.Delete(key);
  }
  for (const auto& kv : key_values) {
    batch.Put(kv.first, kv.second);
  }

  rocksdb::WriteOptions write_options;
  rocksdb::Status status = db_->Write(write_options, &batch);
  if (!status.ok()) {
    LOG_ERROR("Failed to write batch: %s", status.ToString());
  } else {
    LOG_INFO("Data loaded successfully.");
  }
}

rocksdb::Status RedisStore::putKV(const rocksdb::WriteOptions& options, const std::string& key, const std::string& value) {
  rocksdb::Status status = db_->Put(options, key, value);
  return status;
}

rocksdb::Status RedisStore::getKV(const rocksdb::ReadOptions& options, const std::string& key, std::string& value) {
  rocksdb::Status status = db_->Get(options, rocksdb::Slice(key), &value);
  return status;
}

bool RedisStore::get(const std::string& key, std::string& value) {
  // 使用Rocksdb 的ReadOptions
  rocksdb::ReadOptions read_options;
  rocksdb::Status status = db_->Get(read_options, rocksdb::Slice(key), &value);

  if (status.ok()) {
    return true;
  } else if (status.IsNotFound()) {
    return false;
  } else {
    LOG_ERROR("Error reading key %s: %s", key.c_str(), status.ToString().c_str());
    return false;
  }
}


// void RedisStore::recover_from_snapshot(SnapshotDataPtr snap, const StatusCallback& callback) {
//   io_service_.post([this, snap, callback] {
//     std::unordered_map<std::string, std::string> kv; // original kv map

//     // std::vector<std::pair<std::string, std::string>> key_values;  // rocksdb kv pair
//     msgpack::object_handle oh = msgpack::unpack((const char*) snap->data(), snap->size());
//     try {
//       oh.get().convert(kv);
//     } catch (std::exception& e) {
//       callback(Status::io_error("invalid snapshot"));
//       return;
//     }
//     std::swap(kv, key_values_);
//     /** rocksdb, read this into db, using WriteBatch */
    
//     callback(Status::ok());
//   });
// }

// 从snapshot中恢复KV的值，替换现有的kv store. rocksdb version
/**
 * constructor：初始化节点状态：当一个Raft节点启动时，它需要从持久化存储中恢复之前的状态，
 * 包括日志条目和快照数据。构造函数中从快照加载KV的操作，确保节点在启动时能够恢复到之前的状态。
 * recover：更新系统状态：在运行过程中，当系统决定需要创建新的快照或者应用来自其他节点的快照时，
 * 通过从快照中恢复KV来更新系统状态。这样做的目的是确保系统状态的一致性和正确性。
 */
void RedisStore::recover_from_snapshot(SnapshotDataPtr snap, const StatusCallback& callback) {
  io_service_.post([this, snap, callback] {
    std::vector<std::pair<std::string, std::string>> key_values;  // rocksdb kv pair
    msgpack::object_handle oh = msgpack::unpack((const char*) snap->data(), snap->size());
    try {
      oh.get().convert(key_values);
    } catch (std::exception& e) {
      callback(Status::io_error("invalid snapshot"));
      return;
    }
    
    /** rocksdb, read this into db, using WriteBatch */
    load_kv_to_rocksdb(key_values); // 将传过来的data读到db_里了
    callback(Status::ok());
  });
}

// void RedisStore::keys(const char* pattern, int len, std::vector<std::string>& keys) {
//   for (auto it = key_values_.begin(); it != key_values_.end(); ++it) {
//     if (string_match_len(pattern, len, it->first.c_str(), it->first.size(), 0)) {
//       keys.push_back(it->first);
//     }
//   }
// }

// 找到所有符合pattern的key。从现在的KV store中找到pattern对应的所有key，并推到key list中（传入参数）
void RedisStore::keys(const char* pattern, int len, std::vector<std::string>& keys) {
  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_options));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string& key = it->key().ToString();
    if (string_match_len(pattern, len, key.c_str(), key.size(), 0)) {
      keys.push_back(key);
    }
  }

  if (!it->status().ok()) {
    LOG_ERROR("Iterator erros: %s", it->status().ToString().c_str());
  }
}

// void RedisStore::read_commit(proto::EntryPtr entry) {
//   // 定义一个lambda函数，负责处理Raft提交的日志条目。然后传给io_service_去post
//   auto cb = [this, entry] {
//     RaftCommit commit;
//     try {
//       msgpack::object_handle oh = msgpack::unpack((const char*) entry->data.data(), entry->data.size());
//       oh.get().convert(commit);

//     }
//     catch (std::exception& e) {
//       LOG_ERROR("bad entry %s", e.what());
//       return;
//     }
//     RedisCommitData& data = commit.redis_data; // 获得data

//     switch (data.type) {
//       case RedisCommitData::kCommitSet: { // 更新kv数据
//         assert(data.strs.size() == 2);
//         this->key_values_[std::move(data.strs[0])] = std::move(data.strs[1]);
//         break;
//       }
//       case RedisCommitData::kCommitDel: { // 删除key对应的数据
//         for (const std::string& key : data.strs) {
//           this->key_values_.erase(key);
//         }
//         break;
//       }
//       default: {
//         LOG_ERROR("not supported type %d", data.type);
//       }
//     }

//     if (commit.node_id == server_->node_id()) { // 如果commit 由自己发出，在自己的pending中删除该request
//       auto it = pending_requests_.find(commit.commit_id);
//       if (it != pending_requests_.end()) {
//         it->second(Status::ok());
//         pending_requests_.erase(it);
//       }
//     }
//   };

//   io_service_.post(std::move(cb));
// }
/** 处理Raft提交的日志条目，更新kv store，并执行相应的回调函数. 应该在这里处理key value 更新，插入，删除？ */
void RedisStore::read_commit(proto::EntryPtr entry) {
  // 定义一个lambda函数，负责处理Raft提交的日志条目。然后传给io_service_去post
  auto cb = [this, entry] { // 这里是从entry里读到commit 里，data还在commit中。
    RaftCommit commit;
    try {
      msgpack::object_handle oh = msgpack::unpack((const char*) entry->data.data(), entry->data.size());
      oh.get().convert(commit);

    }
    catch (std::exception& e) {
      LOG_ERROR("bad entry %s", e.what());
      return;
    }
    RedisCommitData& data = commit.redis_data; // 获得data

    switch (data.type) {
      case RedisCommitData::kCommitSet: { // 更新kv数据
        assert(data.strs.size() == 2);
       // 只用rocksdb 的put 更新或插入kv pair
        rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), data.strs[0], data.strs[1]);
        if (!status.ok()) {
            LOG_ERROR("Failed to set key-value: %s", status.ToString().c_str());
        }
        break;
      }
      case RedisCommitData::kCommitDel: { // 删除key对应的数据
        for (const std::string& key : data.strs) {
          rocksdb::Status status = db_->Delete(rocksdb::WriteOptions(), key);
          if (!status.ok()) {
            LOG_ERROR("Failed to delete key: %s", status.ToString().c_str());
          }
        }
        break;
      }
      default: {
        LOG_ERROR("not supported type %d", data.type);
      }
    }

    if (commit.node_id == server_->node_id()) { // 如果commit 由自己发出，在自己的pending中删除该request
      auto it = pending_requests_.find(commit.commit_id);
      if (it != pending_requests_.end()) {
        it->second(Status::ok());
        pending_requests_.erase(it);
      }
    }
  };

  io_service_.post(std::move(cb));
}

}

