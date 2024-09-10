#include <boost/algorithm/string.hpp>
#include <raft-kv/transport/peer.h>
#include <raft-kv/common/log.h>
#include <raft-kv/common/bytebuffer.h>
#include <raft-kv/transport/proto.h>
#include <boost/asio.hpp>

namespace kv {

class PeerImpl;
class ClientSession {
 public:
  explicit ClientSession(boost::asio::io_service& io_service, PeerImpl* peer);

  ~ClientSession() {

  }
/** 这个 send 函数的主要目的是将数据打包并放入一个缓冲区中（buffer_），并且在合适的条件下启动数据的发送操作。 */
  void send(uint8_t transport_type, const uint8_t* data, uint32_t len) {
    uint32_t remaining = buffer_.readable_bytes();

    TransportMeta meta;
    meta.type = transport_type;
    meta.len = htonl(len);
    assert(sizeof(TransportMeta) == 5);
    buffer_.put((const uint8_t*) &meta, sizeof(TransportMeta));
    buffer_.put(data, len);
    assert(remaining + sizeof(TransportMeta) + len == buffer_.readable_bytes());

    if (connected_ && remaining == 0) {
      start_write();
    }
  }

  void close_session();
/** 这个 start_connect 函数的作用是启动一个异步的连接过程，尝试与指定的远程端点建立 TCP 连接 (host node)。
 * 函数的设计使用了 Boost.Asio 提供的异步 I/O 操作，并且在连接成功或失败时执行相应的处理逻辑。 */
  void start_connect() {
    socket_.async_connect(endpoint_, [this](const boost::system::error_code& err) {
      if (err) {
        LOG_DEBUG("connect [%lu] error %s in ClientSession::start_connect()", this->peer_id_, err.message().c_str());
        this->close_session();
        return;
      }
      this->connected_ = true;
      LOG_INFO("connected to [%lu] in ClientSession::start_connect()", this->peer_id_);

      if (this->buffer_.readable()) {
        this->start_write();
      }
    });
  }
/** 这个 start_write 函数用于处理异步写操作，将缓冲区中的数据发送到已建立的网络连接中（通过 socket_）。
 * 它的工作原理是检查缓冲区中是否有待发送的数据，如果有，则启动异步写操作，并在写操作完成后继续处理剩余的数据。 */
  void start_write() {
    if (!buffer_.readable()) {
      return;
    }

    uint32_t remaining = buffer_.readable_bytes();
    auto buffer = boost::asio::buffer(buffer_.reader(), remaining);
    auto handler = [this](const boost::system::error_code& error, std::size_t bytes) {
      if (error || bytes == 0) {
        LOG_DEBUG("send [%lu] error %s", this->peer_id_, error.message().c_str());
        this->close_session();
        return;
      }
      this->buffer_.read_bytes(bytes);
      this->start_write();
    };
    boost::asio::async_write(socket_, buffer, handler);
  }

 private:
  boost::asio::ip::tcp::socket socket_;
  boost::asio::ip::tcp::endpoint endpoint_;
  PeerImpl* peer_;
  uint64_t peer_id_;
  ByteBuffer buffer_;
  bool connected_;
};

class PeerImpl : public Peer {
 public:
 /** 在transport::start()中我们实现了host node作为tcp连接的acceptor一方，要根据addr和port实现一个endpoint，
  * 并且start，set，bind和accept这个端口接受的消息。而作为peers，我们只需要连接上这个endpoint就可以了。也就是这里的PeerImp
  */
  explicit PeerImpl(boost::asio::io_service& io_service, uint64_t peer, const std::string& peer_str)
      : peer_(peer),
        io_service_(io_service),
        timer_(io_service) {
    std::vector<std::string> strs;
    boost::split(strs, peer_str, boost::is_any_of(":"));
    if (strs.size() != 2) {
      LOG_DEBUG("invalid host %s", peer_str.c_str());
      exit(0);
    }
    auto address = boost::asio::ip::address::from_string(strs[0]);
    int port = std::atoi(strs[1].c_str());
    endpoint_ = boost::asio::ip::tcp::endpoint(address, port);
  }

  ~PeerImpl() final {
  }

  void start() final {
    start_timer();
  };

  void send(proto::MessagePtr msg) final {
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, *msg);

    do_send_data(TransportTypeStream, (const uint8_t*) sbuf.data(), (uint32_t) sbuf.size());
  }

  void send_snap(proto::SnapshotPtr snap) final {
    LOG_DEBUG("no impl yet");
  }

  void update(const std::string& peer) final {
    LOG_DEBUG("no impl yet");
  }

  uint64_t active_since() final {
    LOG_DEBUG("no impl yet");
    return 0;
  }

  void stop() final {

  }

 private:
 /** 函数的主要作用是发送数据，通过管理与服务器的连接会话（session_），确保数据被正确发送。 */
  void do_send_data(uint8_t type, const uint8_t* data, uint32_t len) {
    if (!session_) {
      session_ = std::make_shared<ClientSession>(io_service_, this);
      session_->send(type, data, len);
      session_->start_connect();
    } else {
      session_->send(type, data, len);
    }
  }
/** 这个函数 start_timer 主要实现了一个定时器（timer）的异步循环调用，并且在每次定时器到期时，
 * 发送调试数据（DebugMessage）到某个目标。 */
  void start_timer() {
    timer_.expires_from_now(boost::posix_time::seconds(3));  //定时器到期时间为3秒
    timer_.async_wait([this](const boost::system::error_code& err) {
      if (err) {
        LOG_ERROR("timer waiter error %s", err.message().c_str());
        return;
      }
      this->start_timer();  // 如果在这个循环中没有错误发生，就会在3秒后调用下一个定时器，形成定时循环机制。
    });

    static std::atomic<uint32_t> tick; // 每次定时器触发，这个计数器都会自增
    DebugMessage dbg;
    dbg.a = tick++;
    dbg.b = tick++;
    do_send_data(TransportTypeDebug, (const uint8_t*) &dbg, sizeof(dbg)); // 传送调试信息
  }

  uint64_t peer_;
  boost::asio::io_service& io_service_;
  friend class ClientSession;
  std::shared_ptr<ClientSession> session_;
  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::deadline_timer timer_;
};

ClientSession::ClientSession(boost::asio::io_service& io_service, PeerImpl* peer)
    : socket_(io_service),
      endpoint_(peer->endpoint_),
      peer_(peer),
      peer_id_(peer_->peer_),
      connected_(false) {

}

void ClientSession::close_session() {
  peer_->session_ = nullptr;
}

std::shared_ptr<Peer> Peer::creat(uint64_t peer, const std::string& peer_str, void* io_service) {
  std::shared_ptr<PeerImpl> peer_ptr(new PeerImpl(*(boost::asio::io_service*) io_service, peer, peer_str));
  return peer_ptr;
}

}
