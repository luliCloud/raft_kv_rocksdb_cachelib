#include <raft-kv/server/redis_session.h>
#include <raft-kv/common/log.h>
#include <unordered_map>
#include <raft-kv/server/redis_store.h>
#include <glib.h>

namespace kv {

#define RECEIVE_BUFFER_SIZE (1024 * 512)

namespace shared {

static const char* ok = "+OK\r\n";
static const char* err = "-ERR %s\r\n";
static const char* wrong_type = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
static const char* unknown_command = "-ERR unknown command `%s`\r\n";
static const char* wrong_number_arguments = "-ERR wrong number of arguments for '%s' command\r\n";
static const char* pong = "+PONG\r\n";
static const char* null = "$-1\r\n";

typedef std::function<void(RedisSessionPtr, struct redisReply* reply)> CommandCallback;

static std::unordered_map<std::string, CommandCallback> command_table = {
    {"ping", RedisSession::ping_command},
    {"PING", RedisSession::ping_command},
    {"get", RedisSession::get_command},
    {"GET", RedisSession::get_command},
    {"set", RedisSession::set_command},
    {"SET", RedisSession::set_command},
    {"del", RedisSession::del_command},
    {"DEL", RedisSession::del_command},
    {"keys", RedisSession::keys_command},
    {"KEYS", RedisSession::keys_command},
};

}
/** 建立会显示在Redis CLI UI的信息 */
static void build_redis_string_array_reply(const std::vector<std::string>& strs, std::string& reply) {
  //*2\r\n$4\r\nkey1\r\n$4key2\r\n

  char buffer[64];
  snprintf(buffer, sizeof(buffer), "*%lu\r\n", strs.size());
  reply.append(buffer);

  for (const std::string& str : strs) {
    snprintf(buffer, sizeof(buffer), "$%lu\r\n", str.size());
    reply.append(buffer);

    if (!str.empty()) {
      reply.append(str);
      reply.append("\r\n");
    }
  }
}

RedisSession::RedisSession(RedisStore* server, boost::asio::io_service& io_service)
    : quit_(false),
      server_(server),
      socket_(io_service),
      read_buffer_(RECEIVE_BUFFER_SIZE),
      reader_(redisReaderCreate()) {
}
/** 启动会话并开始监听来自客户端的消息。RedisStore的调用RedisSession从这里开始
 * 异步操作流程：
程序调用 async_read_some() 函数来非阻塞地从 socket_ 中读取数据。这个操作是异步的，因此程序不会等待数据读取完成，而是继续执行其他代码。
一旦有数据可读或者出现错误，handler 回调函数就会被触发。
在 handler 中，读取到的数据会被处理（调用 handle_read()），如果有错误发生，程序会记录错误日志。
 */
void RedisSession::start() {
  if (quit_) {
    return;
  }
  auto self = shared_from_this(); // 创建一个只想当前对象的sharedptr。异步操作延迟执行，shared-ptr保证该对象在异步操作完成前不会被销毁
  // 存储从客户端读取的数据。这个buffer被传递给async-read-some。表示client端读到的数据会存储到read-buffer-中
  auto buffer = boost::asio::buffer(read_buffer_.data(), read_buffer_.size()); 
  // 异步操作读取。当异步读取完成后，handler会被执行。也就是lambda函数的最后，如果读取成功，那么会执行handle-read这个函数。
  auto handler = [self](const boost::system::error_code& error, size_t bytes) {
    if (bytes == 0) {
      return;
    }
    if (error) {
      LOG_DEBUG("read error %s", error.message().c_str());
      return;
    }

    self->handle_read(bytes);

  };
  socket_.async_read_some(buffer, std::move(handler));
}
/** Redis 客户端接收到的数据的函数。它的任务是解析接收到的 Redis 命令数据，处理协议，生成命令回复，并继续启动下一次读取。 */
void RedisSession::handle_read(size_t bytes) { // byte是从客户端读取到的数据。start，end是指向缓冲区的起始和结束。
  uint8_t* start = read_buffer_.data();
  uint8_t* end = read_buffer_.data() + bytes;
  int err = REDIS_OK;
  std::vector<struct redisReply*> replies;
  // 循环遍历客户端发送的每个完整的命令行，逐步解析数据。
  while (!quit_ && start < end) { 
    /** 使用 memchr() 函数查找 start 到 start + bytes 区间中的第一个换行符 \n。
     * Redis 协议的命令行通常以 \r\n 结尾，所以找到 \n 表示找到了一条完整的 Redis 命令行。 */
    uint8_t* p = (uint8_t*) memchr(start, '\n', bytes);
    /* 如果 p 为 nullptr（即没有找到 \n），那么调用 this->start() 重新开始异步读取，等待更多数据到来，然后退出当前的解析循环。*/
    if (!p) {
      this->start();
      break;
    }

    size_t n = p + 1 - start;
    // 处理完整行。将这行数据（从 start 到 p）传给 Redis 协议解析器 redisReader，进行协议解析。reader- 就是RedisReader
    err = redisReaderFeed(reader_, (const char*) start, n);
    if (err != REDIS_OK) {
      LOG_DEBUG("redis protocol error %d, %s", err, reader_->errstr);
      quit_ = true;
      break;
    }

    struct redisReply* reply = NULL;
    /** 从 redisReader 中获取解析后的命令回复。如果解析成功，reply 会指向一个 redisReply 结构，它表示一个完整的 Redis 命令或命令组的回复。 */
    err = redisReaderGetReply(reader_, (void**) &reply);
    if (err != REDIS_OK) {
      LOG_DEBUG("redis protocol error %d, %s", err, reader_->errstr);
      quit_ = true;
      break;
    }
    if (reply) {
      replies.push_back(reply);
    }

    start += n; // 注意n只是这一行命令的终止。buffer中可能还有下一行命令。所以n不是end
    bytes -= n;
  } // while 循环在这里结束。buffer中所有命令解析完成。
  // 如果解析成功且没有错误。将所有reply放入 replies中。然后for循环完成后，调用新的异步操作，等待下一个命令。
  if (err == REDIS_OK) {
    for (struct redisReply* reply : replies) {
      on_redis_reply(reply);
    }
    this->start();
  }
/** 所有 redisReply 对象在使用完之后都必须被释放。freeReplyObject() 是 hiredis 提供的函数，
 * 用于释放 redisReply 结构体所占用的内存，防止内存泄漏。 */
  for (struct redisReply* reply : replies) {
    freeReplyObject(reply);
  }
}
/** 负责解析从 Redis 客户端接收到的命令并调用对应处理函数的核心函数。
 * 它的作用是检查并验证从客户端发来的 Redis 命令的格式和内容，
 * 然后根据命令名称在命令表中找到相应的回调函数，并执行该回调函数来处理具体的命令。 */
void RedisSession::on_redis_reply(struct redisReply* reply) {
  char buffer[256];
  if (reply->type != REDIS_REPLY_ARRAY) {
    LOG_WARN("wrong type %d", reply->type);
    send_reply(shared::wrong_type, strlen(shared::wrong_type));
    return;
  }

  if (reply->elements < 1) {
    LOG_WARN("wrong elements %lu", reply->elements);
    int n = snprintf(buffer, sizeof(buffer), shared::wrong_number_arguments, "");
    send_reply(buffer, n);
    return;
  }

  if (reply->element[0]->type != REDIS_REPLY_STRING) {
    LOG_WARN("wrong type %d", reply->element[0]->type);
    send_reply(shared::wrong_type, strlen(shared::wrong_type));
    return;
  }

  std::string command(reply->element[0]->str, reply->element[0]->len);
  auto it = shared::command_table.find(command);
  if (it == shared::command_table.end()) {
    int n = snprintf(buffer, sizeof(buffer), shared::unknown_command, command.c_str());
    send_reply(buffer, n);
    return;
  }
  shared::CommandCallback& cb = it->second; // mp中存储了这个command对应的具体函数实现的名字。因此用这个callback就可以异步调用具体的函数实现
  cb(shared_from_this(), reply);
}

void RedisSession::send_reply(const char* data, uint32_t len) {
  uint32_t bytes = send_buffer_.readable_bytes();
  send_buffer_.put((uint8_t*) data, len);
  if (bytes == 0) {
    start_send();
  }
}

void RedisSession::start_send() {
  if (!send_buffer_.readable()) {
    return;
  }
  auto self = shared_from_this();
  uint32_t remaining = send_buffer_.readable_bytes();
  auto buffer = boost::asio::buffer(send_buffer_.reader(), remaining);
  auto handler = [self](const boost::system::error_code& error, std::size_t bytes) {
    if (bytes == 0) {
      return;;
    }
    if (error) {
      LOG_DEBUG("send error %s", error.message().c_str());
      return;
    }
    std::string str((const char*) self->send_buffer_.reader(), bytes);
    self->send_buffer_.read_bytes(bytes);
    self->start_send();
  };
  boost::asio::async_write(socket_, buffer, std::move(handler));
}

void RedisSession::ping_command(std::shared_ptr<RedisSession> self, struct redisReply* reply) {
  self->send_reply(shared::pong, strlen(shared::pong));
}

void RedisSession::get_command(std::shared_ptr<RedisSession> self, struct redisReply* reply) {
  assert(reply->type = REDIS_REPLY_ARRAY);
  assert(reply->elements > 0);
  char buffer[256];

  if (reply->elements != 2) {
    LOG_WARN("wrong elements %lu", reply->elements);
    int n = snprintf(buffer, sizeof(buffer), shared::wrong_number_arguments, "get");
    self->send_reply(buffer, n);
    return;
  }

  if (reply->element[1]->type != REDIS_REPLY_STRING) {
    LOG_WARN("wrong type %d", reply->element[1]->type);
    self->send_reply(shared::wrong_type, strlen(shared::wrong_type));
    return;
  }

  std::string value;
  std::string key(reply->element[1]->str, reply->element[1]->len);
  bool get = self->server_->get(key, value);
  if (!get) {
    self->send_reply(shared::null, strlen(shared::null));
  } else {
    char* str = g_strdup_printf("$%lu\r\n%s\r\n", value.size(), value.c_str());
    self->send_reply(str, strlen(str));
    g_free(str);
  }
}
/** set 就直接调用 server的set功能了。注意在RedisStore中set功能并不是直接set，而是将需要udpate的kv放入msg中，然后交给cluster进行raft 共识
 * 如果raft通过。才调用readcommit中相应的case来处理这个标记为set的commit
 */
void RedisSession::set_command(std::shared_ptr<RedisSession> self, struct redisReply* reply) {
  assert(reply->type = REDIS_REPLY_ARRAY);
  assert(reply->elements > 0);
  char buffer[256];

  if (reply->elements != 3) {
    LOG_WARN("wrong elements %lu", reply->elements);
    int n = snprintf(buffer, sizeof(buffer), shared::wrong_number_arguments, "set");
    self->send_reply(buffer, n);
    return;
  }

  if (reply->element[1]->type != REDIS_REPLY_STRING || reply->element[2]->type != REDIS_REPLY_STRING) {
    LOG_WARN("wrong type %d", reply->element[1]->type);
    self->send_reply(shared::wrong_type, strlen(shared::wrong_type));
    return;
  }
  std::string key(reply->element[1]->str, reply->element[1]->len);
  std::string value(reply->element[2]->str, reply->element[2]->len);
  self->server_->set(std::move(key), std::move(value), [self](const Status& status) {
    if (status.is_ok()) {
      self->send_reply(shared::ok, strlen(shared::ok));
    } else {
      char buff[256];
      int n = snprintf(buff, sizeof(buff), shared::err, status.to_string().c_str());
      self->send_reply(buff, n);
    }
  });
}

void RedisSession::del_command(std::shared_ptr<RedisSession> self, struct redisReply* reply) {
  assert(reply->type = REDIS_REPLY_ARRAY);
  assert(reply->elements > 0);
  char buffer[256];

  if (reply->elements <= 1) { ;
    int n = snprintf(buffer, sizeof(buffer), shared::wrong_number_arguments, "del");
    self->send_reply(buffer, n);
    return;
  }

  std::vector<std::string> keys;
  for (size_t i = 1; i < reply->elements; ++i) {
    redisReply* element = reply->element[i];
    if (element->type != REDIS_REPLY_STRING) {
      self->send_reply(shared::wrong_type, strlen(shared::wrong_type));
      return;
    }

    keys.emplace_back(element->str, element->len);
  }

  self->server_->del(std::move(keys), [self](const Status& status) {
    if (status.is_ok()) {
      self->send_reply(shared::ok, strlen(shared::ok));
    } else {
      char buff[256];
      int n = snprintf(buff, sizeof(buff), shared::err, status.to_string().c_str());
      self->send_reply(buff, n);
    }
  });
}

void RedisSession::keys_command(std::shared_ptr<RedisSession> self, struct redisReply* reply) {
  assert(reply->type = REDIS_REPLY_ARRAY);
  assert(reply->elements > 0);
  char buffer[256];

  if (reply->elements != 2) { ;
    int n = snprintf(buffer, sizeof(buffer), shared::wrong_number_arguments, "keys");
    self->send_reply(buffer, n);
    return;
  }

  redisReply* element = reply->element[1];

  if (element->type != REDIS_REPLY_STRING) {
    self->send_reply(shared::wrong_type, strlen(shared::wrong_type));
    return;
  }

  std::vector<std::string> keys;
  self->server_->keys(element->str, element->len, keys);
  std::string str;
  build_redis_string_array_reply(keys, str);
  self->send_reply(str.data(), str.size());
}

}
