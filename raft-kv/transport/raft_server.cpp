#include <boost/algorithm/string.hpp>
#include <raft-kv/transport/raft_server.h>
#include <raft-kv/common/log.h>
#include <raft-kv/transport/proto.h>
#include <raft-kv/transport/transport.h>
#include <boost/asio.hpp>

namespace kv {

class AsioServer;
class ServerSession : public std::enable_shared_from_this<ServerSession> {
 public:
  explicit ServerSession(boost::asio::io_service& io_service, AsioServer* server)
      : socket(io_service),
        server_(server) {

  }
/** 将传输层的meta data 和message data 读到buffer里 */
  void start_read_meta() {
    assert(sizeof(meta_) == 5); //meta: in proto.cpp TransportMeta instance,
    // 传输层的Meta Data，包括type，len，data[0]
    meta_.type = 0;
    meta_.len = 0;
    auto self = shared_from_this();
    auto buffer = boost::asio::buffer(&meta_, sizeof(meta_)); // 将meta data 读到buffer里
    auto handler = [self](const boost::system::error_code& error, std::size_t bytes) {
      if (bytes == 0) { // from the parameters in the lambda func
        return;;
      }
      if (error) {
        LOG_DEBUG("read error %s", error.message().c_str());
        return;
      }

      if (bytes != sizeof(meta_)) {
        LOG_DEBUG("invalid data len %lu", bytes);
        return;
      }
      self->start_read_message();
    };
// 从socket异步读取数据到buffer，指定了sizeof为完成条件，意思只有在读取完sizeof字节数时，
// 操作才视为完成。 handler是回调函数，当异步读取操作完时被调用。调用先检查meta的数据是否读取正确，
// 再开始读取message
    boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(sizeof(meta_)), handler);
  }
/** 从传输层中读取message，与读取meta data 类似 */
  void start_read_message() {
    uint32_t len = ntohl(meta_.len);
    if (buffer_.capacity() < len) {
      buffer_.resize(len);
    }

    auto self = shared_from_this();
    auto buffer = boost::asio::buffer(buffer_.data(), len);
    auto handler = [self, len](const boost::system::error_code& error, std::size_t bytes) {
      assert(len == ntohl(self->meta_.len));
      if (error || bytes == 0) {
        LOG_DEBUG("read error %s", error.message().c_str());
        return;
      }

      if (bytes != len) {
        LOG_DEBUG("invalid data len %lu, %u", bytes, len);
        return;
      }
      self->decode_message(len);
    };
    boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(len), handler);
  }
/** 根据message 的不同类型进行不同的操作。主要是将message的内容存到对应的变量中。如debug的message
 * 就存到DebugMessage instance中
 */
  void decode_message(uint32_t len) {
    switch (meta_.type) {
      case TransportTypeDebug: {
        assert(len == sizeof(DebugMessage));
        DebugMessage* dbg = (DebugMessage*) buffer_.data();
        assert(dbg->a + 1 == dbg->b);
        //LOG_DEBUG("tick ok");
        break;
      }
      case TransportTypeStream: {
        proto::MessagePtr msg(new proto::Message());
        try {
          msgpack::object_handle oh = msgpack::unpack((const char*) buffer_.data(), len);
          oh.get().convert(*msg);
        }
        catch (std::exception& e) {
          LOG_ERROR("bad message %s, size = %lu, type %s",
                    e.what(),
                    buffer_.size(),
                    proto::msg_type_to_string(msg->type));
          return;
        }
        on_receive_stream_message(std::move(msg));
        break;
      }
      default: {
        LOG_DEBUG("unknown msg type %d, len = %d", meta_.type, ntohl(meta_.len));
        return;
      }
    }

    start_read_meta();
  }

  void on_receive_stream_message(proto::MessagePtr msg);
/**
 * Boost.Asio 是一个跨平台的 C++ 库，用于编程网络和低级 I/O 操作。它提供了丰富的接口来处理异步操作，
 * 使开发者能够构建高效、
 * 可扩展的网络应用程序。这个库是 Boost C++ 库的一部分，广泛应用于需要处理数据传输的软件开发中。
 */
  boost::asio::ip::tcp::socket socket;
 private:
  AsioServer* server_;
  TransportMeta meta_;
  std::vector<uint8_t> buffer_;
};
typedef std::shared_ptr<ServerSession> ServerSessionPtr;

/**
 * AsioServer 类是一个网络服务器实现，利用 Boost.Asio 库进行异步网络操作，
 * 专门用于处理进入的网络连接和消息。该类作为 IoServer 的具体实现，通过继承添加了具体的网络操作功能。
 */
class AsioServer : public IoServer {
 public:
  explicit AsioServer(boost::asio::io_service& io_service,
                      const std::string& host,
                      RaftServer* raft)
      : io_service_(io_service),
        acceptor_(io_service),
        raft_(raft) {
    std::vector<std::string> strs;
    // 注意host指的是服务器自身应该监听的地址和端口号。这不是指向客户端的地址，而是定义了服务器在
    // 网络中的绑定地址，即服务器等待接受客户端连接的位置。
    boost::split(strs, host, boost::is_any_of(":")); // 分解host成两部分ip：port
    if (strs.size() != 2) {
      LOG_DEBUG("invalid host %s", host.c_str());
      exit(0);
    }
    auto address = boost::asio::ip::address::from_string(strs[0]); // 获得host ip
    int port = std::atoi(strs[1].c_str()); // host port
    auto endpoint = boost::asio::ip::tcp::endpoint(address, port);

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(1));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    LOG_DEBUG("listen at %s:%d", address.to_string().c_str(), port);
  }

  ~AsioServer() {

  }
/** 创建一个新的Server Session实例以处理新的连接。所以每个host只有一个acceptor，自己的监听端口
 * 但是可以通过不同的server Session跟不同的client进行连接
 */
  void start() final {
    ServerSessionPtr session(new ServerSession(io_service_, this));
    acceptor_.async_accept(session->socket, [this, session](const boost::system::error_code& error) {
      if (error) {
        LOG_DEBUG("accept error %s", error.message().c_str());
        return;
      }

      this->start();
      session->start_read_meta(); // 递归调用自己以持续监听新的连接
    });
  }
/** 停止io_service_ running, 从而终止所有的异步操作 */
  void stop() final {

  }
/** 该函数将消息传递给RaftServer进行进一步处理，并提供回调以处理任何错误 */
  void on_message(proto::MessagePtr msg) {
    // raft_ is RaftServer, no implement in cpp but defined in header file
    raft_->process(std::move(msg), [](const Status& status) {
      if (!status.is_ok()) {
        LOG_ERROR("process error %s", status.to_string().c_str());
      }
    });
  }

 private:
  boost::asio::io_service& io_service_;
  boost::asio::ip::tcp::acceptor acceptor_; // 用来接受新的TCP连接
  RaftServer* raft_; // 是服务器可以处理给予Raft协议的消息
};

void ServerSession::on_receive_stream_message(proto::MessagePtr msg) {
  server_->on_message(std::move(msg));
}

std::shared_ptr<IoServer> IoServer::create(void* io_service,
                                           const std::string& host,
                                           RaftServer* raft) {
  std::shared_ptr<AsioServer> server(new AsioServer(*(boost::asio::io_service*) io_service, host, raft));
  return server;
}

}
