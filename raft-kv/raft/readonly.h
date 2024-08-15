#pragma once
#include <vector>
#include <raft-kv/raft/proto.h>
#include <raft-kv/raft/config.h>

namespace kv {

// ReadState provides state for read only query.
// It's caller's responsibility to call read_index first before getting
// this state from ready, it's also caller's duty to differentiate if this
// state is what it requests through RequestCtx, eg. given a unique id as
// request_ctx
/** ReadOnly结构在Raft协议中用与管理和处理制度请求。制度请求是在不改变系统状态的情况下从cluster读取数据
 * 的请求。ReadOnly结构确保这些读取操作在整个cluster中是一致且最新的。
 */
struct ReadState {
  bool equal(const ReadState& rs) const { // 比较两个readstate对象是否相等
    if (index != rs.index) {
      return false;
    }
    return request_ctx == rs.request_ctx;
  }
  uint64_t index;
  std::vector<uint8_t> request_ctx;
};

struct ReadIndexStatus {  // 包含只读请求的消息，对应的索引和acks：which nodes已经确认了这个请求
  proto::Message req;
  uint64_t index;
  std::unordered_set<uint64_t> acks;

};
typedef std::shared_ptr<ReadIndexStatus> ReadIndexStatusPtr;

struct ReadOnly {
  explicit ReadOnly(ReadOnlyOption option)
      : option(option) {} // option 指定只读请求的选项，例如安全读或者基于租约读

  // last_pending_request_ctx returns the context of the last pending read only
  // request in readonly struct.
  void last_pending_request_ctx(std::vector<uint8_t>& ctx);

  uint32_t recv_ack(const proto::Message& msg);

  std::vector<ReadIndexStatusPtr> advance(const proto::Message& msg);

  void add_request(uint64_t index, proto::MessagePtr msg);

  ReadOnlyOption option;
  std::unordered_map<std::string, ReadIndexStatusPtr> pending_read_index;
  std::vector<std::string> read_index_queue;
};
typedef std::shared_ptr<ReadOnly> ReadOnlyPtr;

}
/**
 * ReadOnly 的工作流程
添加请求：当接收到一个只读请求时，通过 add_request 方法将请求添加到 ReadOnly 结构中，并初始化请求状态。
处理确认：当节点收到其他节点的确认消息时，通过 recv_ack 方法处理确认，更新请求的确认状态。
一旦一个请求的确认数达到法定人数（quorum），该请求就被视为已完成。
推进请求：当领导者节点发送 MsgReadIndexResp 消息时，通过 advance 方法处理该消息，推进只读请求的状态。
将已完成的请求从待处理队列中移除，并返回这些请求以便进一步处理。

上下文数据在 Raft 协议中用于跟踪和验证只读请求，确保读取操作的一致性和正确性。
上下文数据通常包含请求的标识符、时间戳、节点信息、操作类型和其他附加数据。
ReadOnly 结构通过维护这些上下文数据来管理和处理只读请求，从而在分布式系统中提供一致和可靠的数据读取服务。*/
