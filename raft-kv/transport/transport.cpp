#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <raft-kv/transport/transport.h>
#include <raft-kv/common/log.h>

namespace kv {
// TransportImpl 实现Transport中的所有接口
class TransportImpl : public Transport {

 public:
 // raft_；指向与此传输层相关联的Raf服务器的指针。id_：当前节点的唯一标识符
  explicit TransportImpl(RaftServer* raft, uint64_t id)
      : raft_(raft),
        id_(id) {
  }

  ~TransportImpl() final { // join保证线程完成前主线程不会退出，以及结束后自动退出
    if (io_thread_.joinable()) {
      io_thread_.join();
      LOG_DEBUG("transport stopped");
    }
  }
/** 启动网络服务，包括创建和启动服务器实例，并在新线程中运行IO服务 */
  void start(const std::string& host) final {
    // 注意区分raft_ and server_. server 是服务器实例
    server_ = IoServer::create((void*) &io_service_, host, raft_);
    server_->start();

    io_thread_ = std::thread([this]() {
      this->io_service_.run();
    });
  }
/** 将新的同伴节点加到peers map中，注意每个peers_指与该节点相连的所有节点 */
  void add_peer(uint64_t id, const std::string& peer) final {
    LOG_DEBUG("node:%lu, peer:%lu, addr:%s", id_, id, peer.c_str());
    std::lock_guard<std::mutex> guard(mutex_);

    auto it = peers_.find(id);
    if (it != peers_.end()) {
      LOG_DEBUG("peer already exists %lu", id);
      return;
    }

    PeerPtr p = Peer::creat(id, peer, (void*) &io_service_);  // 开始其他peer node和host的连接
    p->start();  // 开始定时器
    peers_[id] = p;  // 加入host node的peers map
  }

  void remove_peer(uint64_t id) final {
    LOG_WARN("no impl yet");
  }
/** 异步发送一系列消息给同伴节点 */
  void send(std::vector<proto::MessagePtr> msgs) final {
    auto callback = [this](std::vector<proto::MessagePtr> msgs) {
      for (proto::MessagePtr& msg : msgs) {
        if (msg->to == 0) { // 如果peers id 为0
          // ignore intentionally dropped message
          continue;
        }

        auto it = peers_.find(msg->to);
        if (it != peers_.end()) {
          it->second->send(msg);
          continue;
        }
        LOG_DEBUG("ignored message %d (sent to unknown peer %lu)", msg->type, msg->to);
      }
    };
    io_service_.post(std::bind(callback, std::move(msgs)));
  }

  void stop() final {
    io_service_.stop();
  }

 private:
  RaftServer* raft_;
  uint64_t id_;

  std::thread io_thread_;
  boost::asio::io_service io_service_;

  std::mutex mutex_;
  std::unordered_map<uint64_t, PeerPtr> peers_; // 储存与当前节点连接的所有节点的id。所以每个节点都拥有自己的peers map

  IoServerPtr server_;
};

std::shared_ptr<Transport> Transport::create(RaftServer* raft, uint64_t id) {
  std::shared_ptr<TransportImpl> impl(new TransportImpl(raft, id));
  return impl;
}

}
