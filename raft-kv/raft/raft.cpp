#include <boost/algorithm/string.hpp>
#include <raft-kv/raft/raft.h>
#include <raft-kv/common/log.h>
#include <raft-kv/common/slice.h>
#include <raft-kv/raft/util.h>
/** RaftNode，Node（RawNode），Raft三者的关系，就是Node是Raft的高级封装，RaftNode是Node的更高级封装 */
namespace kv {

static const std::string kCampaignPreElection = "CampaignPreElection";
static const std::string kCampaignElection = "CampaignElection";
static const std::string kCampaignTransfer = "CampaignTransfer";
// 记录有几个需要变更的配置
static uint32_t num_of_pending_conf(const std::vector<proto::EntryPtr>& entries) {
  uint32_t n = 0;
  for (const proto::EntryPtr& entry: entries) {
    if (entry->type == proto::EntryConfChange) {
      n++;
    }
  }
  return n;
}

Raft::Raft(const Config& c)
    : id_(c.id),
      term_(0),
      vote_(0),
      max_msg_size_(c.max_size_per_msg),
      max_uncommitted_size_(c.max_uncommitted_entries_size),
      max_inflight_(c.max_inflight_msgs),
      state_(RaftState::Follower),
      is_learner_(false),
      lead_(0),
      lead_transferee_(0),
      pending_conf_index_(0),
      uncommitted_size_(0),
      read_only_(new ReadOnly(c.read_only_option)),
      election_elapsed_(0),
      heartbeat_elapsed_(0),
      check_quorum_(c.check_quorum),
      pre_vote_(c.pre_vote),
      heartbeat_timeout_(c.heartbeat_tick),
      election_timeout_(c.election_tick),
      randomized_election_timeout_(0),
      disable_proposal_forwarding_(c.disable_proposal_forwarding),
      random_device_(0, c.election_tick) {
  raft_log_ = std::make_shared<RaftLog>(c.storage, c.max_committed_size_per_ready);
  proto::HardState hs;
  proto::ConfState cs;
  Status status = c.storage->initial_state(hs, cs);
  if (!status.is_ok()) {
    LOG_FATAL("%s", status.to_string().c_str());
  }

  std::vector<uint64_t> peers = c.peers;
  std::vector<uint64_t> learners = c.learners;

// 这段代码主要确保初始化Raft节点时，不能同时指定新的Raft节点列表（peers 和 learners）以及从快照中
// 恢复的配置状态（ConfState的nodes和learners），避免配置冲突。
  if (!cs.nodes.empty() || !cs.learners.empty()) { // 如果非空表示需要从快照中恢复配置
    if (!peers.empty() || !learners.empty()) { // 检查Raft初始化时是否已经指定了peers和leaners
      // tests; the argument should be removed and these tests should be
      // updated to specify their nodes through a snapshot.
      LOG_FATAL("cannot specify both newRaft(peers, learners) and ConfState.(Nodes, Learners)");
    }
    peers = cs.nodes; // 没有指定，就从快照中恢复
    learners = cs.learners;
  }

  for (uint64_t peer : peers) {
    ProgressPtr p(new Progress(max_inflight_));
    p->next = 1;
    prs_[peer] = p;
  }

  for (uint64_t learner :  learners) {
    auto it = prs_.find(learner);
    if (it != prs_.end()) {
      LOG_FATAL("node %lu is in both learner and peer list", learner);
    }

    ProgressPtr p(new Progress(max_inflight_));
    p->next = 1;
    p->is_learner = true;

    learner_prs_[learner] = p;

    if (id_ == learner) {
      is_learner_ = true; // 说明这个Raft就是一个learner
    }
  }

  if (!hs.is_empty_state()) {
    load_state(hs);
  }

  if (c.applied > 0) { // 已经应用的日志idx
    raft_log_->applied_to(c.applied);

  }
  become_follower(term_, 0); // 该节点成为follower

  std::string node_str;
  {
    std::vector<std::string> nodes_strs;
    std::vector<uint64_t> node;
    this->nodes(node); // nodes()是Raft类中的函数。将该Raft节点的所有从节点的idx存进node 里
    for (uint64_t n : node) {
      nodes_strs.push_back(std::to_string(n));
    }
    node_str = boost::join(nodes_strs, ",");
  }

  LOG_INFO("raft %lu [peers: [%s], term: %lu, commit: %lu, applied: %lu, last_index: %lu, last_term: %lu]",
           id_,
           node_str.c_str(),
           term_,
           raft_log_->committed_,
           raft_log_->applied_,
           raft_log_->last_index(),
           raft_log_->last_term());
}

Raft::~Raft() {

}

void Raft::become_follower(uint64_t term, uint64_t lead) {
  // 使用bind将step-follower绑定为当前对象的成员函数，并将其绑定到step-回调函数上。
  // 这意味着收到新消息时，会调用step- follower来处理这些消息
  step_ = std::bind(&Raft::step_follower, this, std::placeholders::_1);
  reset(term); // 重置节点的任期为传入的term，包括重置选举计时器等

  tick_ = std::bind(&Raft::tick_election, this); // 每次tick时，会调用tick- election处理选举超时

  lead_ = lead;
  state_ = RaftState::Follower;

  LOG_INFO("%lu became follower at term %lu", id_, term_);
}

void Raft::become_candidate() {
  if (state_ == RaftState::Leader) {
    LOG_FATAL("invalid transition [leader -> candidate]");
  }
  step_ = std::bind(&Raft::step_candidate, this, std::placeholders::_1);
  reset(term_ + 1); // 变为candidate时 自动给任期加1
  tick_ = std::bind(&Raft::tick_election, this); // 使用tick- election处理选举超时
  vote_ = id_; // 给自己id投一票
  state_ = RaftState::Candidate;
  LOG_INFO("%lu became candidate at term %lu", id_, term_);
}

void Raft::become_pre_candidate() { // 成为预候选人时，只会改变step func和state，不改变其他（包括任期）
  if (state_ == RaftState::Leader) {
    LOG_FATAL("invalid transition [leader -> pre-candidate]");
  }
  // Becoming a pre-candidate changes our step functions and state,
  // but doesn't change anything else. In particular it does not increase
  // r.Term or change r.Vote.
  step_ = std::bind(&Raft::step_candidate, this, std::placeholders::_1);
  votes_.clear(); // 投票清空
  tick_ = std::bind(&Raft::tick_election, this);
  lead_ = 0; // lead 为不存在的节点
  state_ = RaftState::PreCandidate;
  LOG_INFO("%lu became pre-candidate at term %lu", id_, term_);
}

void Raft::become_leader() {
  if (state_ == RaftState::Follower) {
    LOG_FATAL("invalid transition [follower -> leader]");
  }
  step_ = std::bind(&Raft::step_leader, this, std::placeholders::_1);

  reset(term_); // 将任期改为自己的term
  tick_ = std::bind(&Raft::tick_heartbeat, this);
  lead_ = id_;  // lead也是自己
  state_ = RaftState::Leader;
  // Followers enter replicate mode when they've been successfully probed
  // (perhaps after having received a snapshot as a result). The leader is
  // trivially in this state. Note that r.reset() has initialized this
  // progress with the last index already.
  auto it = prs_.find(id_); // 在progress中找到自己的id
  assert(it != prs_.end());
  it->second->become_replicate();  // progress->become_replicate。leader为自己也保持一个progress
  // 对象，却阿波进度管理逻辑的一致性。并保证为复制模式（定时向follower发送心跳消息进行消息复制）

  // Conservatively set the pendingConfIndex to the last index in the
  // log. There may or may not be a pending config change, but it's
  // safe to delay any future proposals until we commit all our
  // pending log entries, and scanning the entire tail of the log
  // could be expensive.
  pending_conf_index_ = raft_log_->last_index();

  auto empty_ent = std::make_shared<proto::Entry>();

  if (!append_entry(std::vector<proto::Entry>{*empty_ent})) {
    // This won't happen because we just called reset() above.
    LOG_FATAL("empty entry was dropped");
  }

  // As a special case, don't count the initial empty entry towards the
  // uncommitted log quota. This is because we want to preserve the
  // behavior of allowing one entry larger than quota if the current
  // usage is zero.
  std::vector<proto::EntryPtr> entries{empty_ent};
  reduce_uncommitted_size(entries);
  LOG_INFO("%lu became leader at term %lu", id_, term_);
}

void Raft::campaign(const std::string& campaign_type) {
  uint64_t term = 0;
  proto::MessageType vote_msg = 0;
  if (campaign_type == kCampaignPreElection) {
    become_pre_candidate();
    vote_msg = proto::MsgPreVote;
    // PreVote RPCs are sent for the next term before we've incremented r.Term.
    // 这种pre candidate模式通常是在进行真正选举前先发起一轮问询，确保大多数node会给自己投票，再
    // 增加任期号，正式发起vote request
    term = term_ + 1;
  } else {
    become_candidate(); // 在这个函数里term已经+1，所以后面不需要加了
    vote_msg = proto::MsgVote;
    term = term_;
  }

  if (quorum() == poll(id_, vote_resp_msg_type(vote_msg), true)) { // 投票占1/2以上为quorum
    // We won the election after voting for ourselves (which must mean that
    // this is a single-node cluster). Advance to the next state.
    if (campaign_type == kCampaignPreElection) {
      campaign(kCampaignElection);
    } else {
      become_leader();
    }
    return;
  }

  for (auto it = prs_.begin(); it != prs_.end(); ++it) {
    if (it->first == id_) { // 如果是自己 则不需要发送vote request
      continue;
    }

    LOG_INFO("%lu [log_term: %lu, index: %lu] sent %s request to %lu at term %lu",
             id_,
             raft_log_->last_term(),
             raft_log_->last_index(),
             proto::msg_type_to_string(vote_msg),
             it->first,
             term_);

    std::vector<uint8_t> ctx;
    if (campaign_type == kCampaignTransfer) {
      ctx = std::vector<uint8_t>(kCampaignTransfer.begin(), kCampaignTransfer.end());
    }
    proto::MessagePtr msg(new proto::Message());
    msg->term = term;
    msg->to = it->first;
    msg->type = vote_msg;
    msg->index = raft_log_->last_index();
    msg->log_term = raft_log_->last_term();
    msg->context = std::move(ctx);
    send(std::move(msg)); // 发送vote request。注意跟现实在屏幕的LOG- INFO不是同一个obj，尽管他们的内容是相似的
  }
}
/** 用于记录和统计选票。在Raft协议的选举过程中，追踪哪些Node投票支持当前node成为leader */
uint32_t Raft::poll(uint64_t id, proto::MessageType type, bool v) {
  uint32_t granted = 0;
  if (v) { // v is for vote
    LOG_INFO("%lu received %s from %lu at term %lu", id_, proto::msg_type_to_string(type), id, term_);
  } else {
    LOG_INFO("%lu received %s rejection from %lu at term %lu", id_, proto::msg_type_to_string(type), id, term_);
  }
// 如果这个节点（id代表）还没有投票，将投票的vector该id改为投票
  auto it = votes_.find(id);
  if (it == votes_.end()) {
    votes_[id] = v; 
  }
// 记录所有的投票数。iterate的votes- vector
  for (it = votes_.begin(); it != votes_.end(); ++it) {
    if (it->second) {
      granted++;
    }
  }
  return granted;
}

/** 这段代码是Raft协议中处理消息的核心函数。该函数根据收到的消息类型和节点状态，（通常来自node。cpp）
 * 决定如何处理这些消息 */
Status Raft::step(proto::MessagePtr msg) {
  if (msg->term == 0) { // 任期为0，不做任何处理。

  } else if (msg->term > term_) { // 更高任期的消息（大于该节点的任期）
    if (msg->type == proto::MsgVote || msg->type == proto::MsgPreVote) {
      // 检查消息的上下文数据等于kCampaign Transfer，which 表示一次强制投票请求。
      bool force = (Slice((const char*) msg->context.data(), msg->context.size()) == Slice(kCampaignTransfer));
      // 检查租约是否过期： check- quorum是否启用租约检查。lead！=0 当前节点是否知道领导者
      // election：选举超时时间是否未超过。
      bool in_lease = (check_quorum_ && lead_ != 0 && election_elapsed_ < election_timeout_);
      if (!force && in_lease) { // 如果不是强制投票且当前租约未过期，忽略该投票请求，记录相关信息。返回成功状态。
        // If a server receives a RequestVote request within the minimum election timeout
        // of hearing from a current leader, it does not update its term or grant its vote
        LOG_INFO(
            "%lu [log_term: %lu, index: %lu, vote: %lu] ignored %s from %lu [log_term: %lu, index: %lu] at term %lu: lease is not expired (remaining ticks: %d)",
            id_,
            raft_log_->last_term(),
            raft_log_->last_index(),
            vote_,
            proto::msg_type_to_string(msg->type),
            msg->from,
            msg->log_term,
            msg->index,
            term_,
            election_timeout_ - election_elapsed_);
        return Status::ok();
      } 
    }
    switch (msg->type) { // 注意这个switch仍然在这个任期更高的case里。现在处理不是租约的情况
      case proto::MsgPreVote:
        // Never change our term in response to a PreVote
        break;
      case proto::MsgPreVoteResp:
      // 这是处理预投票响应消息的部分
        if (!msg->reject) { // msg-》reject为false表示没有被拒绝。没有被拒绝执行以下逻辑
        // 节点发送预投票请求时，会使用一个比当前人气更高的人气。这是为了在预投票成功后能安全的进入
        // candidate状态。如果预投票成功，节点获得quorum即法定票数，会增加它的人气并正式发起选举
        // 如果失败，拒绝预投票的节点会提供一个较高的人气，当前节点应当根据这个新的加工熬人气成为follower
          // We send pre-vote requests with a term in our future. If the
          // pre-vote is granted, we will increment our term when we get a
          // quorum. If it is not, the term comes from the node that
          // rejected our vote so we should become a follower at the new
          // term.
          break;
        }
      default:LOG_INFO("%lu [term: %lu] received a %s message with higher term from %lu [term: %lu]",
                       id_, term_,
                       proto::msg_type_to_string(msg->type),
                       msg->from,
                       msg->term);

        if (msg->type == proto::MsgApp || msg->type == proto::MsgHeartbeat || msg->type == proto::MsgSnap) {
          become_follower(msg->term, msg->from);
        } else {
          become_follower(msg->term, 0); // 成为没有领导者的状态，因为不是从leader发来的消息
        }
    }
  } else if (msg->term < term_) { // 如果我们有更低任期的消息
  // 如果我们有来自更低任期的领导者的消息。检查是否启用了检查法定人数或预投票
    if ((check_quorum_ || pre_vote_) && (msg->type == proto::MsgHeartbeat || msg->type == proto::MsgApp)) {
      // We have received messages from a leader at a lower term. It is possible
      // that these messages were simply delayed in the network, but this could
      // also mean that this node has advanced its term number during a network
      // partition, and it is now unable to either win an election or to rejoin
      // the majority on the old term. If checkQuorum is false, this will be
      // handled by incrementing term numbers in response to MsgVote with a
      // higher term, but if checkQuorum is true we may not advance the term on
      // MsgVote and must generate other messages to advance the term. The net
      // result of these two features is to minimize the disruption caused by
      // nodes that have been removed from the cluster's configuration: a
      // removed node will send MsgVotes (or MsgPreVotes) which will be ignored,
      // but it will not receive MsgApp or MsgHeartbeat, so it will not create
      // disruptive term increases, by notifying leader of this node's activeness.
      // The above comments also true for Pre-Vote
      //
      // When follower gets isolated, it soon starts an election ending
      // up with a higher term than leader, although it won't receive enough
      // votes to win the election. When it regains connectivity, this response
      // with "pb.MsgAppResp" of higher term would force leader to step down.
      // However, this disruption is inevitable to free this stuck node with
      // fresh election. This can be prevented with Pre-Vote phase.
      /** 我们收到了来自低任期领导者的消息。这些消息可能仅仅是因为网络延迟。但也可能意味着这个节点（this）
       * 在网络分区期间提升了自己的任期号。现在它无法赢得选举或在旧任期上重新加入大多数。如果
       * checkQuorun为false，leader不会通过检查是否有法定人数来确定自己是否还是领导者。这样可以防止
       * 频繁的领导变更，但是也会让cluster的领导处于不合法工作的危险。leader会通过更高任期的响应来更新的任期号到更高。
       * 如果启用了checkQuorun，则leader会随时检查自己的合法性。一旦收到来自比自己任期更高的response，会
       * 自动将自己的状态变为follower并增加任期数。
       * 这两个特性的净结果是最小化被移出集群配置的节点所带来的干扰：
       * 上述评论对PreVote也适合。
       * 
       * 当跟随者被隔离时，它很快就会启动一次选举，结果是它的任期比领导者高。尽管他不会获得足够的选票赢得选举
       * 当他重新获得连接时，这种具有更高任期的pb.MesAppResp响应（resp是response）将迫使领导者下台。然而
       * 这种干扰是不可避免的，以通过新的选举来解放这个陷入困境的节点，这可以通过Pre-vote来防止
       */
      // 一旦启用了任期检查或者在prevote中，就需要将自己的任期号发给sender，通知他们自己的任期号更高，
      // 前者迫使旧领导下台。后者迫使发起request的node意识到自己不合法。
      proto::MessagePtr m(new proto::Message());
      m->to = msg->from;
      m->type = proto::MsgAppResp; // respsonse
      send(std::move(m)); // 给它回应
    } else if (msg->type == proto::MsgPreVote) {
      // Before Pre-Vote enable, there may have candidate with higher term,
      // but less log. After update to Pre-Vote, the cluster may deadlock if
      // we drop messages with a lower term.
      // 如果消息是预投票类型，生成一个拒绝消息发送回去。日志记录拒绝的原因和关系
      LOG_INFO(
          "%lu [log_term: %lu, index: %lu, vote: %lu] rejected %s from %lu [log_term: %lu, index: %lu] at term %lu",
          id_,
          raft_log_->last_term(),
          raft_log_->last_index(),
          vote_,
          proto::msg_type_to_string(msg->type),
          msg->from,
          msg->log_term,
          msg->index,
          term_);
      proto::MessagePtr m(new proto::Message());
      m->to = msg->from;
      m->type = proto::MsgPreVoteResp;
      m->reject = true;
      m->term = term_;
      send(std::move(m));
    } else { // 选择忽略
      // ignore other cases
      LOG_INFO("%lu [term: %lu] ignored a %s message with lower term from %lu [term: %lu]",
               id_, term_, proto::msg_type_to_string(msg->type), msg->from, msg->term);
    }
    return Status::ok();
  }
// 以上三种任期情况有term==0和term_<term 的情况中有没有return的情况，都进入这个case。
// 发回response并进一步处理
  switch (msg->type) {
    case proto::MsgHup: { // 触发新一轮选举。
      if (state_ != RaftState::Leader) {
        std::vector<proto::EntryPtr> entries;
        // 获取未应用的日志条目。调用slice从applied + 1 到 committed+1 的范围内的未应用日志条目
        //（但已提交）到entries中
        Status status =
            raft_log_->slice(raft_log_->applied_ + 1,
                             raft_log_->committed_ + 1,
                             RaftLog::unlimited(),
                             entries);
        if (!status.is_ok()) {
          LOG_FATAL("unexpected error getting unapplied entries (%s)", status.to_string().c_str());
        }

        uint32_t pending = num_of_pending_conf(entries); // 如果存在未应用的配置变更（会从entries）
        // 中判断是否有，并且有已已提交但未应用的日志条目，则记录警告信息并返回，不发起选举。
        if (pending > 0 && raft_log_->committed_ > raft_log_->applied_) {
          LOG_WARN(
              "%lu cannot campaign at term %lu since there are still %u pending configuration changes to apply",
              id_,
              term_,
              pending);
          return Status::ok();
        }
        LOG_INFO("%lu is starting a new election at term %lu", id_, term_);
        if (pre_vote_) {
          campaign(kCampaignPreElection);
        } else {
          campaign(kCampaignElection);
        }
      } else {
        LOG_DEBUG("%lu ignoring MsgHup because already leader", id_);
      }
      break;
    }
    case proto::MsgVote:
    case proto::MsgPreVote: {
      // 两种vote都进入这个
      // TODO: learner may need to vote, in case of node down when conf change.
      // 在该版本中我们没有实现learner的vote，因为我们默认它不投票。在将来的版本里可能要实现。
      if (is_learner_) {
        LOG_INFO(
            "%lu [log_term: %lu, index: %lu, vote: %lu] ignored %s from %lu [log_term: %lu, index: %lu] at term %lu: learner can not vote",
            id_,
            raft_log_->last_term(),
            raft_log_->last_index(),
            vote_,
            proto::msg_type_to_string(msg->type),
            msg->from,
            msg->log_term,
            msg->index,
            msg->term);
        return Status::ok();
      }
      // 判断是否可以投票。
      // We can vote if this is a repeat of a vote we've already cast...
      // 如果vote-等于msg from说明当前节点已经投给msg来源的节点，可以再次投票给该来源
      bool can_vote = vote_ == msg->from ||
          // ...we haven't voted and we don't think there's a leader yet in this term...
          // 未投票切当前任期没有领导者
          (vote_ == 0 && lead_ == 0) ||
          // ...or this is a PreVote for a future term...
          // 消息类型是预投票切请求的任期大于当前节点任期，可以投票。
          (msg->type == proto::MsgPreVote && msg->term > term_);
      // ...and we believe the candidate is up to date.
      // 如果发起投票的节点的日志index和任期数至少跟当前节点一致，并且检查了可以投票。
      // log- term，一条日志自带的它属于的任期号。term：当前节点所属于的任期。
      if (can_vote && this->raft_log_->is_up_to_date(msg->index, msg->log_term)) { 
        // 投票给请求者
        LOG_INFO(
            "%lu [log_term: %lu, index: %lu, vote: %lu] cast %s for %lu [log_term: %lu, index: %lu] at term %lu",
            id_,
            raft_log_->last_term(),
            raft_log_->last_index(),
            vote_,
            proto::msg_type_to_string(msg->type),
            msg->from,
            msg->log_term,
            msg->index,
            term_);
        // When responding to Msg{Pre,}Vote messages we include the term
        // from the message, not the local term. To see why consider the
        // case where a single node was previously partitioned away and
        // it's local term is now of date. If we include the local term
        // (recall that for pre-votes we don't update the local term), the
        // (pre-)campaigning node on the other end will proceed to ignore
        // the message (it ignores all out of date messages).
        // The term in the original message and current local term are the
        // same in the case of regular votes, but different for pre-votes.

        proto::MessagePtr m(new proto::Message());
        m->to = msg->from;
        m->term = msg->term;
        m->type = vote_resp_msg_type(msg->type); // 生成投票请求。这里rejct为false
        send(std::move(m));

        if (msg->type == proto::MsgVote) { // 只有在正式投票时记录投票
          // Only record real votes.
          election_elapsed_ = 0; // 重启投票时间为0，将来和election timeout比较
          vote_ = msg->from;  // 记录投票给哪个节点
        }
      } else { // 如果不能投票或者任期号，日志号，日志任期号不对
        LOG_INFO(
            "%lu [log_term: %lu, index: %lu, vote: %lu] rejected %s from %lu [log_term: %lu, index: %lu] at term %lu",
            id_,
            raft_log_->last_term(),
            raft_log_->last_index(),
            vote_,
            proto::msg_type_to_string(msg->type),
            msg->from,
            msg->log_term,
            msg->index,
            term_);

        proto::MessagePtr m(new proto::Message());
        m->to = msg->from;
        m->term = term_;
        m->type = vote_resp_msg_type(msg->type);
        m->reject = true;  // 这里将即将发送的消息的reject设置为true，也就是拒绝投票。这是拒绝的关键
        send(std::move(m));
      }

      break;
    }
    default: {
      return step_(msg);
    }
  }

  return Status::ok();
}
/** 这个函数主要用于leader节点处理不同类型的消息 */
Status Raft::step_leader(proto::MessagePtr msg) {
  // These message types do not require any progress for m.From.
  switch (msg->type) {
    case proto::MsgBeat: {
      bcast_heartbeat();
      return Status::ok();
    }
    case proto::MsgCheckQuorum:
      if (!check_quorum_active()) {
        LOG_WARN("%lu stepped down to follower since quorum is not active", id_);
        become_follower(term_, 0);
      }
      return Status::ok();
    case proto::MsgProp: { // 处理客户端提案消息。如果为空，记录致命错误。
      if (msg->entries.empty()) {
        LOG_FATAL("%lu stepped empty MsgProp", id_);
      }
      auto it = prs_.find(id_);
      if (it == prs_.end()) {  // 如果这个节点不在peers中，说明已经被移除出cluster。拒绝提案
        // If we are not currently a member of the range (i.e. this node
        // was removed from the configuration while serving as leader),
        // drop any new proposals.
        return Status::invalid_argument("raft proposal dropped");
      }

      if (lead_transferee_ != 0) { // 如果transferee不为空，说明在移交领导权中（从该节点移给transferee，不接受提案
        LOG_DEBUG("%lu [term %lu] transfer leadership to %lu is in progress; dropping proposal",
                  id_,
                  term_,
                  lead_transferee_);
        return Status::invalid_argument("raft proposal dropped");
      }
// 处理其他节点提交的正常提案 MsgPro，包括ConfChange
      for (size_t i = 0; i < msg->entries.size(); ++i) {
        proto::Entry& e = msg->entries[i];
        if (e.type == proto::EntryConfChange) { // 如果提案类型为ConfChange
        // 如果自身节点Pending的conf idx大于已经应用的log index。忽略提案，先处理自己ing
          if (pending_conf_index_ > raft_log_->applied_) {
            LOG_INFO(
                "propose conf %s ignored since pending unapplied configuration [index %lu, applied %lu]",
                proto::entry_type_to_string(e.type),
                pending_conf_index_,
                raft_log_->applied_);
            e.type = proto::EntryNormal;
            e.index = 0;
            e.term = 0;
            e.data.clear();
          } else { // pending conf 已经应用。则把这个pros加上。因为要iterate所有i，所以自然将i加上再加1（可能提案从1开始）
            pending_conf_index_ = raft_log_->last_index() + i + 1;
          }
        }
      }

      if (!append_entry(msg->entries)) {
        return Status::invalid_argument("raft proposal dropped");
      }
      bcast_append();
      return Status::ok();
    }
    case proto::MsgReadIndex: { // 处理读索引消息。根据不同的读选项，安全度或基于租约读
    // 如果当前cluster法定人数大于1或者小于等于1（后者直接添加读状态，返回status ok）
      if (quorum() > 1) { 
        uint64_t term = 0;
        raft_log_->term(raft_log_->committed_, term); // 获取当前日志条目对应的任期号，赋值给term
        if (term != term_) { // 如果已经应用的日志的任期号与当前领导者的任期号不一致，不处理。因为log有问题。不能提供log读服务
          return Status::ok();
        }

        // thinking: use an interally defined context instead of the user given context.
        // We can express this in terms of the term and index instead of a user-supplied value.
        // This would allow multiple reads to piggyback on the same message.
        switch (read_only_->option) { // 根据选项处理请求。
        // 对于安全读选项，首先添加读请求。然后广播带有上下文的心跳信息。这种方式确保读请求在大多数节点上都被处理。保证一致性
        /** 领导者会向所有追随者发送心跳消息，并等待大多数追随者的响应。
一旦收到大多数追随者的响应，领导者就可以确定它仍然是合法的领导者，并且它的日志是最新的。
然后，领导者可以处理读请求，确保返回的数据是一致且最新的。 */  
          case ReadOnlySafe:read_only_->add_request(raft_log_->committed_, msg);
            bcast_heartbeat_with_ctx(msg->entries[0].data);
            break;
          case ReadOnlyLeaseBased:
          // 如果在租约中，leader不需要向其他节点确认自己的日志权威，可以直接处理消息。
            if (msg->from == 0 || msg->from == id_) { // from local member（自己）。直接将读状态添加到read-states-
              read_states_
                  .push_back(ReadState{.index = raft_log_->committed_, .request_ctx = msg->entries[0]
                      .data});
            } else {  // 否则创建一个响应消息并发送给请求的节点。
  // 注意这个响应消息包含的是一个日志索引， 并且被大多数节点确认并持久化的最新log index。而不是实际数据
  // 客户端可以使用这个索引来确认数据的一致性，并给予这个索引来读取数据。而无需担心数据是否是一致且最新的。
              proto::MessagePtr m(new proto::Message());
              m->to = msg->from;
              m->type = proto::MsgReadIndexResp;
              m->index = raft_log_->committed_;
              m->entries = msg->entries;
              send(std::move(m));
            }
            break;
        }
      } else { // 法定人数不大于1的请求，直接将读状态添加到read-states
        read_states_.push_back(ReadState{.index = raft_log_->committed_, .request_ctx = msg->entries[0].data});
      }

      return Status::ok();
    }
  }

  // All other message types require a progress for m.From (pr).
  auto pr = get_progress(msg->from); // 获取发送消息的follower的进度信息
  if (pr == nullptr) {
    LOG_DEBUG("%lu no progress available for %lu", id_, msg->from);
    return Status::ok();
  }
  switch (msg->type) {
    // 日志追加响应消息。在分布式系统中，leader通过发送日志追加消息将日志条目复制到follower，当follower
    // 接收到这些消息并处理后，会发送响应消息回leader。follower根据这些响应来调整自身状态和follower的进度
    case proto::MsgAppResp: {
      pr->recent_active = true; // 设置最近活跃状态为true

      if (msg->reject) { // 如果follower拒绝了日志追加请求。leader会调整该follower的进度
        LOG_DEBUG("%lu received msgApp rejection(last_index: %lu) from %lu for index %lu",
                  id_, msg->reject_hint, msg->from, msg->index);
        // 如果follower的进度可以更新，则将其状态从复制replicated变为探测，找到leader和follower一致的index并从那里开始复制日志
        if (pr->maybe_decreases_to(msg->index, msg->reject_hint)) { 
          LOG_DEBUG("%lu decreased progress of %lu to [%s]", id_, msg->from, pr->string().c_str());
          if (pr->state == ProgressStateReplicate) {
            pr->become_probe();
          }
          send_append(msg->from);
        }
      } else { // 如果follower接受了日志追加请求，leader会更新该follower的进度。
        bool old_paused = pr->is_paused(); // 获取之前follower是否处于暂停状态
        if (pr->maybe_update(msg->index)) {
          if (pr->state == ProgressStateProbe) { // 检查进度是否从probe变为replicate
            pr->become_replicate();
          } else if (pr->state == ProgressStateSnapshot && pr->need_snapshot_abort()) {
            // 如果进度处于快照状态且需要中止快照，则改为probe状态。
            LOG_DEBUG("%lu snapshot aborted, resumed sending replication messages to %lu [%s]",
                      id_,
                      msg->from,
                      pr->string().c_str());
            // Transition back to replicating state via probing state
            // (which takes the snapshot into account). If we didn't
            // move to replicating state, that would only happen with
            // the next round of appends (but there may not be a next
            // round for a while, exposing an inconsistent RaftStatus).
            pr->become_probe();
          } else if (pr->state == ProgressStateReplicate) {
            // 如果进度处于复制状态，释放飞行中的消息？
            pr->inflights->free_to(msg->index);
          }

          if (maybe_commit()) { // 如果可以提交日志条目，则广播提交消息。broadcast-》bcast
            bcast_append();
          } else if (old_paused) { // 如果处于暂停状态，重新发送日志追加消息。
            // If we were paused before, this node may be missing the
            // latest commit index, so send it.
            send_append(msg->from);
          }
          // We've updated flow control information above, which may
          // allow us to send multiple (size-limited) in-flight messages
          // at once (such as when transitioning from probe to
          // replicate, or when freeTo() covers multiple messages). If
          // we have more entries to send, send as many messages as we
          // can (without sending empty messages for the commit index)
          // 检查是否有更多的日志条目可以发送，并发送这些条目 （在while条件里同时发送和判断）
          while (maybe_send_append(msg->from, false)) {
          }
          // Transfer leadership is in progress.
          // 如果该follower是leadership转移的target node👎进度已经与leader的日志完全同步，
          // 发送超时消息完成领导权转移。
          if (msg->from == lead_transferee_ && pr->match == raft_log_->last_index()) {
            LOG_INFO("%lu sent MsgTimeoutNow to %lu after received MsgAppResp", id_, msg->from);
            send_timeout_now(msg->from);
          }
        }
      }
    }
      break;
    case proto::MsgHeartbeatResp: { // 心跳回复消息，从follower发送过来
      pr->recent_active = true;
      pr->resume();

      // free one slot for the full inflights window to allow progress.
      if (pr->state == ProgressStateReplicate && pr->inflights->is_full()) {
        pr->inflights->free_first_one();
      }
      if (pr->match < raft_log_->last_index()) {
        send_append(msg->from);
      }

      if (read_only_->option != ReadOnlySafe || msg->context.empty()) {
        return Status::ok();
      }

      uint32_t ack_count = read_only_->recv_ack(*msg); // acknowledge count
      if (ack_count < quorum()) {
        return Status::ok();
      }

      auto rss = read_only_->advance(*msg);
      for (auto& rs : rss) {
        auto& req = rs->req;
        if (req.from == 0 || req.from == id_) { // 从自己来，local消息，将读消息push进读状态
          ReadState read_state = ReadState{.index = rs->index, .request_ctx = req.entries[0].data};
          read_states_.push_back(std::move(read_state));
        } else {
          proto::MessagePtr m(new proto::Message());
          m->to = req.from;
          m->type = proto::MsgReadIndexResp;
          m->index = rs->index;
          m->entries = req.entries;
          send(std::move(m)); // 回复follower日志条目（已确认过一致，否则发送appendMsg）
        }
      }
    }
      break;
    case proto::MsgSnapStatus: {
      if (pr->state != ProgressStateSnapshot) {
        return Status::ok();
      }
      if (!msg->reject) { // 如果更新？快照状态没有被拒绝
        pr->become_probe();
        LOG_DEBUG("%lu snapshot succeeded, resumed sending replication messages to %lu [%s]",
                  id_,
                  msg->from,
                  pr->string().c_str());
      } else {
        pr->snapshot_failure();
        pr->become_probe();
        LOG_DEBUG("%lu snapshot failed, resumed sending replication messages to %lu [%s]",
                  id_,
                  msg->from,
                  pr->string().c_str());
      }
      // If snapshot finish, wait for the msgAppResp from the remote node before sending
      // out the next msgApp.确认快照已经更新再发送下一个消息
      // If snapshot failure, wait for a heartbeat interval before next try。等待一个心跳周期再尝试
      pr->set_pause();
      break;
    }
    case proto::MsgUnreachable: {
      // During optimistic replication, if the remote becomes unreachable,
      // there is huge probability that a MsgApp is lost.
      if (pr->state == ProgressStateReplicate) {
        pr->become_probe();
      }
      LOG_DEBUG("%lu failed to send message to %lu because it is unreachable [%s]", id_,
                msg->from,
                pr->string().c_str());
      break;
    }
    case proto::MsgTransferLeader: {
      if (pr->is_learner) {
        LOG_DEBUG("%lu is learner. Ignored transferring leadership", id_);
        return Status::ok();
      }

      uint64_t lead_transferee = msg->from; // 发送消息的node是被移交者
      uint64_t last_lead_transferee = lead_transferee_; // 该leader记录的需要被移交的及诶单
      if (last_lead_transferee != 0) { // 如果已经有一个正在转移的领导权（记录在leader中）
        if (last_lead_transferee == lead_transferee) { // 信息比对一致
        // 说明之前已经有一个正在进行的领导权转移，并且新的转移请求的目标与之前一致，则忽略新的请求，return
          LOG_INFO(
              "%lu [term %lu] transfer leadership to %lu is in progress, ignores request to same node %lu",
              id_,
              term_,
              lead_transferee,
              lead_transferee);
          return Status::ok();
        }
        abort_leader_transfer(); // 信息不一致，中止之前的领导权转移，而不是继续
        LOG_INFO("%lu [term %lu] abort previous transferring leadership to %lu",
                 id_,
                 term_,
                 last_lead_transferee);
      }
      if (lead_transferee == id_) {
        LOG_DEBUG("%lu is already leader. Ignored transferring leadership to self", id_);
        return Status::ok();
      }
      // Transfer leadership to third party.记录日志
      LOG_INFO("%lu [term %lu] starts to transfer leadership to %lu", id_, term_, lead_transferee);
      // Transfer leadership should be finished in one electionTimeout, so reset r.electionElapsed.
      // 将election elapsed重置为0，这样可以再一个选举超时时间内完成领导权转移。记录新的目标转移节点
      election_elapsed_ = 0;
      lead_transferee_ = lead_transferee;
      if (pr->match == raft_log_->last_index()) { // 如果目标节点日志已经跟领导同步，立即发送TimeOutNow
      // 这会触发目标节点立即发送选举，成为新的领导者。
        send_timeout_now(lead_transferee);
        LOG_INFO("%lu sends MsgTimeoutNow to %lu immediately as %lu already has up-to-date log",
                 id_,
                 lead_transferee,
                 lead_transferee);
      } else {
        send_append(lead_transferee); // 如果日志没有完全同步。则发送消息进行日志同步
      }
      break;
    }
  }
  return Status::ok();
}
/** 这个函数实现了candidate状态下处理消息的逻辑。即该节点为candidate */
Status Raft::step_candidate(proto::MessagePtr msg) {
  // Only handle vote responses corresponding to our candidacy (while in
  // StateCandidate, we may get stale MsgPreVoteResp messages in this term from
  // our pre-candidate state).
  switch (msg->type) {
    // 如果收到提案，记录信息并丢弃提案，因为候选者无法处理提案。返回invalid表示提案被丢弃
    case proto::MsgProp:LOG_INFO("%lu no leader at term %lu; dropping proposal", id_, term_);
      return Status::invalid_argument("raft proposal dropped");
    // 如果收到日志追加消息，会退回到追随者状态，因为一个有效的领导者存在，调用become follower方法，处理日志追加消息
    case proto::MsgApp:become_follower(msg->term, msg->from); // always m.Term == r.Term
      handle_append_entries(std::move(msg));
      break;
    case proto::MsgHeartbeat:become_follower(msg->term, msg->from);  // always m.Term == r.Term
      handle_heartbeat(std::move(msg));
      break;
    case proto::MsgSnap:become_follower(msg->term, msg->from); // always m.Term == r.Term
      handle_snapshot(std::move(msg));
      break;
      // 处理预投票和响应投票消息
    case proto::MsgPreVoteResp:
    case proto::MsgVoteResp: {
      uint64_t gr = poll(msg->from, msg->type, !msg->reject); // 调用poll 记录投票结果
      // 记录赞成和拒绝的比例
      LOG_INFO("%lu [quorum:%u] has received %lu %s votes and %lu vote rejections",
               id_,
               quorum(),
               gr,
               proto::msg_type_to_string(msg->type),
               votes_.size() - gr);
      if (quorum() == gr) { // 如果达到法定人数
        if (state_ == RaftState::PreCandidate) { // 如果是Pre Candidate，发起选举
          campaign(kCampaignElection);
        } else { // 否则就是candidate，变为领导者，广播
          assert(state_ == RaftState::Candidate);
          become_leader();
          bcast_append();
        }
      } else if (quorum() == votes_.size() - gr) { // 如果拒绝是大多数
        // pb.MsgPreVoteResp contains future term of pre-candidate
        // m.Term > r.Term; reuse r.Term
        become_follower(term_, 0);
      }
      break;
    }
    case proto::MsgTimeoutNow: { // 收到超时消息，忽略，因为候选人不需要处理超时消息。
      LOG_DEBUG("%lu [term %lu state %d] ignored MsgTimeoutNow from %lu",
                id_,
                term_,
                state_,
                msg->from);
    }
  }
  return Status::ok();
}
/** 该函数用于发送消息。确保消息格式的正确性，并将消息添加到待发送消息队列中 */
void Raft::send(proto::MessagePtr msg) {
  msg->from = id_;
  if (msg->type == proto::MsgVote || msg->type == proto::MsgVoteResp || msg->type == proto::MsgPreVote
      || msg->type == proto::MsgPreVoteResp) {
    if (msg->term == 0) { // 消息中的term不能为0，因为下一个选举周期一定不是从0开始
      // All {pre-,}campaign messages need to have the term set when
      // sending.
      // - MsgVote: m.Term is the term the node is campaigning for,
      //   non-zero as we increment the term when campaigning.
      // - MsgVoteResp: m.Term is the new r.Term if the MsgVote was
      //   granted, non-zero for the same reason MsgVote is
      // - MsgPreVote: m.Term is the term the node will campaign,
      //   non-zero as we use m.Term to indicate the next term we'll be
      //   campaigning for
      // - MsgPreVoteResp: m.Term is the term received in the original
      //   MsgPreVote if the pre-vote was granted, non-zero for the
      //   same reasons MsgPreVote is
      LOG_FATAL("term should be set when sending %s", proto::msg_type_to_string(msg->type));

    }
  } else { // 不是投票信息，则term应该为0
    if (msg->term != 0) {
      LOG_FATAL("term should not be set when sending %d (was %lu)", msg->type, msg->term);
    }
    // do not attach term to MsgProp, MsgReadIndex
    // proposals are a way to forward to the leader and
    // should be treated as local message.
    // MsgReadIndex is also forwarded to leader.
    // 对于提案消息和读索引消息，不设置term，以为他们该被视为本地消息，并转发给领导者
    if (msg->type != proto::MsgProp && msg->type != proto::MsgReadIndex) {
      msg->term = term_; // 对于其他非投票相关消息，设置消息的term为当前节点的term
    }
  }
  msgs_.push_back(std::move(msg)); // 添加消息到消息队列中
}
/** 根据其他节点信息恢复本节点状态 */
void Raft::restore_node(const std::vector<uint64_t>& nodes, bool is_learner) {
  for (uint64_t node: nodes) {
    uint64_t match = 0;
    uint64_t next = raft_log_->last_index() + 1;
    if (node == id_) {
      match = next - 1;
      is_learner_ = is_learner;
    }
    set_progress(node, match, next, is_learner);
    LOG_INFO("%lu restored progress of %lu [%s]", id_, node, get_progress(id_)->string().c_str());
  }
}
/** 检查该节点是否可以被提升到leader */
bool Raft::promotable() const {
  auto it = prs_.find(id_);
  return it != prs_.end();
}
/** 添加一个节点或learner。注意这个函数处理不一定是本节点，而是其他节点的状态转变 */
void Raft::add_node_or_learner(uint64_t id, bool is_learner) {
  ProgressPtr pr = get_progress(id); // 可能不是本节点。
  if (pr == nullptr) {
    set_progress(id, 0, raft_log_->last_index() + 1, is_learner);
  } else {

    if (is_learner && !pr->is_learner) { // 不能将普通节点变为learner，只能从learner变为voter
      // can only change Learner to Voter
      LOG_INFO("%lu ignored addLearner: do not support changing %lu from raft peer to learner.", id_, id);
      return;
    }

    if (is_learner == pr->is_learner) { // 如果节点的当前状态与目标状态一致，忽略重复的操作。
      // Ignore any redundant addNode calls (which can happen because the
      // initial bootstrapping entries are applied twice).
      return;
    }

    // change Learner to Voter, use origin Learner progress
    // 如果将学习node变为普通node，删除学习者节点中该节点，并更新状态为普通
    learner_prs_.erase(id);
    pr->is_learner = false;
    prs_[id] = pr;
  }

  if (id_ == id) {
    is_learner_ = is_learner;
  }

  // When a node is first added, we should mark it as recently active.
  // Otherwise, CheckQuorum may cause us to step down if it is invoked
  // before the added node has a chance to communicate with us.
  get_progress(id)->recent_active = true;
}
/** 删除节点 */
void Raft::remove_node(uint64_t id) {
  del_progress(id);

  // do not try to commit or abort transferring if there is no nodes in the cluster.
  if (prs_.empty() && learner_prs_.empty()) {
    return;
  }

  // The quorum size is now smaller, so see if any pending entries can
  // be committed.
  if (maybe_commit()) {
    bcast_append();
  }
  // If the removed node is the leadTransferee, then abort the leadership transferring.
  if (state_ == RaftState::Leader && lead_transferee_ == id) {
    abort_leader_transfer();
  }
}
/** follower状态下处理接收到的各种类型的消息。只有日志append，心跳，快照append，或者要求该节点成为leader时才对调用
 * 需要的函数进行msg handle。否则都是将该msg转发给leader
 */
Status Raft::step_follower(proto::MessagePtr msg) {
  switch (msg->type) {
    case proto::MsgProp:
      if (lead_ == 0) {
        LOG_INFO("%lu no leader at term %lu; dropping proposal", id_, term_);
        return Status::invalid_argument("raft proposal dropped");
      } else if (disable_proposal_forwarding_) { // 如果禁用了提案转发
        LOG_INFO("%lu not forwarding to leader %lu at term %lu; dropping proposal", id_, lead_, term_);
        return Status::invalid_argument("raft proposal dropped");
      }
      msg->to = lead_; // 发给领导者
      send(msg);
      break;
    case proto::MsgApp: { // MsgApp是Msg Append
      election_elapsed_ = 0; // 重置选举超时时间
      lead_ = msg->from; // 从leader接收到append log 消息
      handle_append_entries(msg); // 处理log append
      break;
    }
    case proto::MsgHeartbeat: {
      election_elapsed_ = 0;
      lead_ = msg->from;
      handle_heartbeat(msg);
      break;
    }
    case proto::MsgSnap: {
      election_elapsed_ = 0;
      lead_ = msg->from;
      handle_snapshot(msg);
      break;
    }
    case proto::MsgTransferLeader:
    // 领导权转移消息
      if (lead_ == 0) {
        LOG_INFO("%lu no leader at term %lu; dropping leader transfer msg", id_, term_);
        return Status::ok();
      }
      msg->to = lead_;
      send(msg); // 将消息的目标设置为领导并发送
      break;
    case proto::MsgTimeoutNow:
    // 立即超时消息
      if (promotable()) { // 如果当前节点 可被提升为leader，记录日志并立即开始选举
        LOG_INFO("%lu [term %lu] received MsgTimeoutNow from %lu and starts an election to get leadership.",
                 id_,
                 term_,
                 msg->from);
        // Leadership transfers never use pre-vote even if r.preVote is true; we
        // know we are not recovering from a partition so there is no need for the
        // extra round trip.
        campaign(kCampaignTransfer);
      } else {
        LOG_INFO("%lu received MsgTimeoutNow from %lu but is not promotable", id_, msg->from);
      }
      break;
    case proto::MsgReadIndex:
      if (lead_ == 0) {
        LOG_INFO("%lu no leader at term %lu; dropping index reading msg", id_, term_);
        return Status::ok();
      }
      msg->to = lead_;
      send(msg);
      break;
    case proto::MsgReadIndexResp:
    // 该消息用于响应MsgReadIndex消息。MsgRead Index是客户端请求 Raft集群读取特定数据时发送的消息
    // 而Resp是领导者节点响应读取请求并提供的请求日志的索引。该消息包含了读取请求的上下文信息（ctx）和
    // 已提交的日志索引。将这些信息添加到follower read-state中，可以帮助follower跟踪和管理这些读取请求
      if (msg->entries.size() != 1) {
        LOG_ERROR("%lu invalid format of MsgReadIndexResp from %lu, entries count: %lu",
                  id_,
                  msg->from,
                  msg->entries.size());
        return Status::ok();
      }
      ReadState rs; // ReadState用于跟踪已经处理的只读请求，保持cluster的数据一致性
      rs.index = msg->index;
      rs.request_ctx = std::move(msg->entries[0].data);
      read_states_.push_back(std::move(rs));
      break;
  }
  return Status::ok();
}
/** 这个函数用处理追加日志条目的消息。主要是更新follower日志，与leader保持一致，并根据处理结果向leader发送response msg */
void Raft::handle_append_entries(proto::MessagePtr msg) {
  if (msg->index < raft_log_->committed_) { // 如果日志idx比本节点已提交的log idx小。
  // 发送response给leader，通知它自己已经提交的最新log idx
    proto::MessagePtr m(new proto::Message());
    m->to = msg->from;
    m->type = proto::MsgAppResp;
    m->index = raft_log_->committed_;
    send(std::move(m));
    return;
  }

  std::vector<proto::EntryPtr> entries;
  for (proto::Entry& entry: msg->entries) {
    entries.push_back(std::make_shared<proto::Entry>(std::move(entry)));
  }

  bool ok = false;
  uint64_t last_index = 0;
  // 调用maybe append函数尝试追加日志条目，last index和OK应该会在添加过程中改变。如果添加成功。
  // maybe append函数会尝试将不冲突的日志都添加到本地日志中，并更新最新的日志idx和返回添加是否成功（OK）
  // 该节点的raft-log中也会记录这些更新。
  raft_log_->maybe_append(msg->index, msg->log_term, msg->commit, std::move(entries), last_index, ok);

  if (ok) {
    proto::MessagePtr m(new proto::Message());
    m->to = msg->from;
    m->type = proto::MsgAppResp;
    m->index = last_index;
    send(std::move(m));
  } else {
    uint64_t term = 0;
    raft_log_->term(msg->index, term);
    LOG_DEBUG("%lu [log_term: %lu, index: %lu] rejected msgApp [log_term: %lu, index: %lu] from %lu",
              id_, term, msg->index, msg->log_term, msg->index, msg->from)

    proto::MessagePtr m(new proto::Message());
    m->to = msg->from;
    m->type = proto::MsgAppResp;
    m->index = msg->index;
    m->reject = true;
    m->reject_hint = raft_log_->last_index();
    send(std::move(m));
  }

}

void Raft::handle_heartbeat(proto::MessagePtr msg) {
  raft_log_->commit_to(msg->commit);
  proto::MessagePtr m(new proto::Message());
  m->to = msg->from;
  m->type = proto::MsgHeartbeatResp;
  msg->context = std::move(msg->context);
  send(std::move(m));
}
/** 根据msg的快照恢复状态。在restore函数中会检查snapshot的最新idx是否比自己committed的log idx大 */
void Raft::handle_snapshot(proto::MessagePtr msg) {
  uint64_t sindex = msg->snapshot.metadata.index; // 记录snapshot的idx和term
  uint64_t sterm = msg->snapshot.metadata.term;

  if (restore(msg->snapshot)) { // 日志信息。本节点，最后提交在log idx。恢复为快照在日志idx，term的状态
    LOG_INFO("%lu [commit: %lu] restored snapshot [index: %lu, term: %lu]",
             id_, raft_log_->committed_, sindex, sterm);
    proto::MessagePtr m(new proto::Message());
    m->to = msg->from;
    m->type = proto::MsgAppResp;
    msg->index = raft_log_->last_index();
    send(std::move(m));
  } else {
    LOG_INFO("%lu [commit: %lu] ignored snapshot [index: %lu, term: %lu]",
             id_, raft_log_->committed_, sindex, sterm);
    proto::MessagePtr m(new proto::Message());
    m->to = msg->from;
    m->type = proto::MsgAppResp;
    msg->index = raft_log_->committed_;
    send(std::move(m));
  }

}

bool Raft::restore(const proto::Snapshot& s) {
  if (s.metadata.index <= raft_log_->committed_) { // 如果snapshot的日志idx比自己小，则忽略msg
    return false;
  }
// 如果符合snapshot的idx和term，将该节点的状态快进到snapshot的状态，return false（不需要应用snapshot）
/** 当快照的索引和任期与当前日志的索引和任期匹配时，说明当前节点的日志已经包含了快照中的所有日志条目。
 * 这种情况下，可以直接将节点的提交索引更新到快照的索引（日志已经全部存在，但可能还没提交。更改一下commit idx即可），
 * 这样做效率更高，因为不需要重复应用已经存在的日志条目。*/
  if (raft_log_->match_term(s.metadata.index, s.metadata.term)) {
    LOG_INFO(
        "%lu [commit: %lu, last_index: %lu, last_term: %lu] fast-forwarded commit to snapshot [index: %lu, term: %lu]",
        id_,
        raft_log_->committed_,
        raft_log_->last_index(),
        raft_log_->last_term(),
        s.metadata.index,
        s.metadata.term);
    raft_log_->commit_to(s.metadata.index);
    return false;
  }

  // The normal peer can't become learner.
  if (!is_learner_) {
    for (uint64_t id : s.metadata.conf_state.learners) {
      if (id == id_) {
        LOG_ERROR("%lu can't become learner when restores snapshot [index: %lu, term: %lu]",
                  id_,
                  s.metadata.index,
                  s.metadata.term);
        return false;
      }

    }
  }
/** 需要将所有log应用到当前节点中，所以需要snapshot中更多信息，与之前match的情况不一样 */
  LOG_INFO("%lu [commit: %lu, last_index: %lu, last_term: %lu] starts to restore snapshot [index: %lu, term: %lu]",
           id_,
           raft_log_->committed_,
           raft_log_->last_index(),
           raft_log_->last_term(),
           s.metadata.index,
           s.metadata.term);

  proto::SnapshotPtr snap(new proto::Snapshot(s));
  raft_log_->restore(snap);
  prs_.clear();
  learner_prs_.clear();
  restore_node(s.metadata.conf_state.nodes, false);
  restore_node(s.metadata.conf_state.learners, true);
  return true; // return true表示应用了snapshot
}

void Raft::tick() {
  if (tick_) {
    /** tick_() 是一个函数调用，意味着 tick_ 是一个可调用对象
     * （例如函数指针、std::function 对象或一个具有 operator() 的类对象）。调用 tick_() 会执行 tick_ 所指向的函数
     * tick_ 在raft node初始化的时候一定是assign成follower.但是在后续的raft中，可能会更改成leader，candidate，从而
     * 发生不一样的行为。
     * tick_ 是一个函数对象，通常用 std::function<void()> 来表示。这意味着 tick_ 可以存储一个不带参数且无返回值的函数或 lambda 表达式。
tick_() 的调用相当于执行 tick_ 绑定的具体函数。这个函数可能是心跳超时处理函数、
选举超时处理函数或者领导者心跳发送函数。 */
    tick_();
  } else {
    LOG_WARN("tick function is not set");
  }
}
/** soft state包括当前任期的leader和本节点的状态（follower， leader，candidate，precandidate） */
SoftStatePtr Raft:: soft_state() const {
  return std::make_shared<SoftState>(lead_, state_);// state-即Raft State，当前节点在cluster中的角色
}
/** hard state包括当前任期，该节点投票给谁的ID，最新提交的日志 */
proto::HardState Raft::hard_state() const {
  proto::HardState hs;
  hs.term = term_;
  hs.vote = vote_;
  hs.commit = raft_log_->committed_;
  return hs;
}

void Raft::load_state(const proto::HardState& state) {
  // 第二种情况说明还有一些日志在该节点不存在，需要先将该节点日志同步到最新状态。第一种说明节点状态比
  // 传入的state还要新，不需要更新状态。
  if (state.commit < raft_log_->committed_ || state.commit > raft_log_->last_index()) {
    LOG_FATAL("%lu state.commit %lu is out of range [%lu, %lu]",
              id_,
              state.commit,
              raft_log_->committed_,
              raft_log_->last_index());
  }
  raft_log_->committed_ = state.commit;
  term_ = state.term;
  vote_ = state.vote;
}
// 根据该node的peers情况更新Node vector（应该是用于获取cluster的peers情况）
void Raft::nodes(std::vector<uint64_t>& node) const {
  for (auto it = prs_.begin(); it != prs_.end(); ++it) {
    node.push_back(it->first);
  }
  std::sort(node.begin(), node.end());
}

void Raft::learner_nodes(std::vector<uint64_t>& learner) const {
  for (auto it = learner_prs_.begin(); it != prs_.end(); ++it) {
    learner.push_back(it->first);
  }
  std::sort(learner.begin(), learner.end());
}
/** 获取node id的progress。 Progress对象包含了该Follower Node的日志复制进展，
 * 包括Next Index（当前最新日志idx + 1）和 matchIndex（与leader相同的最后一个idx） */
ProgressPtr Raft::get_progress(uint64_t id) {
  auto it = prs_.find(id);
  if (it != prs_.end()) {
    return it->second;
  }

  it = learner_prs_.find(id);
  if (it != learner_prs_.end()) {
    return it->second;
  }
  return nullptr;
}

void Raft::set_progress(uint64_t id, uint64_t match, uint64_t next, bool is_learner) {
  if (!is_learner) {
    learner_prs_.erase(id);
    ProgressPtr progress(new Progress(max_inflight_));
    progress->next = next;
    progress->match = match;
    prs_[id] = progress;  // 加入prs map 中
    return;
  }
// 该id在传入中设定为learner，但是我们在peers中找到了它。所以它应该本来就是voter。不需要设定。
  auto it = prs_.find(id);
  if (it != prs_.end()) {
    LOG_FATAL("%lu unexpected changing from voter to learner for %lu", id_, id);
  }

  ProgressPtr progress(new Progress(max_inflight_));
  progress->next = next;
  progress->match = match;
  progress->is_learner = true;

  learner_prs_[id] = progress;  // 将learner转为voter
}

void Raft::del_progress(uint64_t id) {
  prs_.erase(id);
  learner_prs_.erase(id);
}

void Raft::send_append(uint64_t to) {
  maybe_send_append(to, true);
}
/** 根据节点的进度向指定的节点发送日志追加消息或快照消息。通过这种方式，raft协议可以保证所有节点的日志一致 */
bool Raft::maybe_send_append(uint64_t to, bool send_if_empty) {
  ProgressPtr pr = get_progress(to); // 从 to Node获得progress
  if (pr->is_paused()) {
    return false;
  }
// 这里我们要根据Process判断是否还有新的日志条目需要发送给to Node
  proto::MessagePtr msg(new proto::Message());
  msg->to = to;
  uint64_t term = 0;
  Status status_term = raft_log_->term(pr->next - 1, term); // 获得to Node的进度中next索引的日志的任期
  std::vector<proto::EntryPtr> entries; // 获得从next索引开始的log，最多max msg size条
  // 这里是update restart node 的核心函数。从raft——log获得从restart node的最后log开始的下一条的所有log，并加进entries
  Status status_entries = raft_log_->entries(pr->next, max_msg_size_, entries);
  if (entries.empty() && !send_if_empty) {  // 如果没有日志条目需要发送且不强制发送空消息
    return false;
  }
// 如果获取日志条目或任期失败，则检查目标节点是否最近活跃。如果不活跃，则不发送快照消息。
  if (!status_term.is_ok() || !status_entries.is_ok()) { // send snapshot if we failed to get term or entries
    if (!pr->recent_active) {
      LOG_DEBUG("ignore sending snapshot to %lu since it is not recently active", to)
      return false;
    }

    msg->type = proto::MsgSnap;

    proto::SnapshotPtr snap;
    Status status = raft_log_->snapshot(snap);
    if (!status.is_ok()) {
      LOG_FATAL("snapshot error %s", status.to_string().c_str());
    }
    if (snap->is_empty()) {
      LOG_FATAL("need non-empty snapshot");
    }
    uint64_t sindex = snap->metadata.index;
    uint64_t sterm = snap->metadata.term;
    LOG_DEBUG("%lu [first_index: %lu, commit: %lu] sent snapshot[index: %lu, term: %lu] to %lu [%s]",
              id_, raft_log_->first_index(), raft_log_->committed_, sindex, sterm, to, pr->string().c_str());
    pr->become_snapshot(sindex); // 更新目标节点的状态为快照状态
    msg->snapshot = *snap;
    LOG_DEBUG("%lu paused sending replication messages to %lu [%s]", id_, to, pr->string().c_str());
  } else {  // 如果获得日志条目或者任期成功
    msg->type = proto::MsgApp;
    msg->index = pr->next - 1;
    msg->log_term = term;
    for (proto::EntryPtr& entry: entries) { // 在这里将entries中的所有日志加到msg->entries 中。这个msg之后就会发给从节点，要求其同步日志。
      //copy
      msg->entries.emplace_back(*entry);
    }

    msg->commit = raft_log_->committed_;
    if (!msg->entries.empty()) {
      switch (pr->state) {
        // optimistically increase the next when in ProgressStateReplicate
        case ProgressStateReplicate: { // 复制状态。可以进行更新
          uint64_t last = msg->entries.back().index;
          pr->optimistic_update(last);
          pr->inflights->add(last);
          break;
        }
        case ProgressStateProbe: { // 探索状态
          pr->set_pause();
          break;
        }
        default: {
          LOG_FATAL("%lu is sending append in unhandled state %s", id_, progress_state_to_string(pr->state));
        }
      }
    }
  }
  send(std::move(msg));
  return true;
}
/** 用于向指定的节点发送心跳消息。 */
void Raft::send_heartbeat(uint64_t to, std::vector<uint8_t> ctx) {
  // Attach the commit as min(to.matched, r.committed).
  // When the leader sends out heartbeat message,
  // the receiver(follower) might not be matched with the leader
  // or it might not have all the committed entries.
  // The leader MUST NOT forward the follower's commit to
  // an unmatched index.
  // 获取目标节点的进度信息。通过get progress获取to节点的匹配索引。将心跳消息的提交索引设置为目标节点
  // 的匹配索引和领导者已提交索引中的最小值。这确保了领导者不会将未匹配的索引提交给追随者。
  uint64_t commit = std::min(get_progress(to)->match, raft_log_->committed_);
  proto::MessagePtr msg(new proto::Message());
  msg->to = to;
  msg->type = proto::MsgHeartbeat;
  msg->commit = commit;
  msg->context = std::move(ctx);
  send(std::move(msg));
}

void Raft::for_each_progress(const std::function<void(uint64_t, ProgressPtr&)>& callback) {
  for (auto it = prs_.begin(); it != prs_.end(); ++it) {
    callback(it->first, it->second);
  }

  for (auto it = learner_prs_.begin(); it != learner_prs_.end(); ++it) {
    callback(it->first, it->second);
  }
}
// 广播append
void Raft::bcast_append() {
  for_each_progress([this](uint64_t id, ProgressPtr& progress) {
    if (id == id_) {
      return;
    }
    this->send_append(id);
  });
}

void Raft::bcast_heartbeat() {
  std::vector<uint8_t> ctx;
  read_only_->last_pending_request_ctx(ctx);
  bcast_heartbeat_with_ctx(std::move(ctx));
}

void Raft::bcast_heartbeat_with_ctx(const std::vector<uint8_t>& ctx) {
  for_each_progress([this, ctx](uint64_t id, ProgressPtr& progress) {
    if (id == id_) {
      return;
    }

    this->send_heartbeat(id, std::move(ctx));
  });
}

bool Raft::maybe_commit() {
  // Preserving matchBuf across calls is an optimization
  // used to avoid allocating a new slice on each call.
  match_buf_.clear();

  for (auto it = prs_.begin(); it != prs_.end(); ++it) {
    match_buf_.push_back(it->second->match);
  }
  std::sort(match_buf_.begin(), match_buf_.end());
  auto mci = match_buf_[match_buf_.size() - quorum()];
  return raft_log_->maybe_commit(mci, term_);
}
/** 重置整个节点的状态到初始 */
void Raft::reset(uint64_t term) {
  if (term_ != term) {
    term_ = term;
    vote_ = 0;
  }
  lead_ = 0;

  election_elapsed_ = 0;
  heartbeat_elapsed_ = 0;
  reset_randomized_election_timeout();

  abort_leader_transfer();

  votes_.clear();
  for_each_progress([this](uint64_t id, ProgressPtr& progress) {
    bool is_learner = progress->is_learner;
    progress = std::make_shared<Progress>(max_inflight_);
    progress->next = raft_log_->last_index() + 1;
    progress->is_learner = is_learner;

    if (id == id_) {
      progress->match = raft_log_->last_index();
    }

  });

  pending_conf_index_ = 0;
  uncommitted_size_ = 0;
  read_only_->pending_read_index.clear();
  read_only_->read_index_queue.clear();
}

void Raft::add_node(uint64_t id) {
  add_node_or_learner(id, false);
}

bool Raft::append_entry(const std::vector<proto::Entry>& entries) {
  uint64_t li = raft_log_->last_index(); // 最后一个日志索引
  std::vector<proto::EntryPtr> ents(entries.size(), nullptr);
// 将传入的entries添加到本Node 的log entry中。append在已有的最后一个日志后
  for (size_t i = 0; i < entries.size(); ++i) {
    proto::EntryPtr ent(new proto::Entry());
    ent->term = term_;
    ent->index = li + 1 + i;
    ent->data = entries[i].data;
    ent->type = entries[i].type;
    ents[i] = ent;
  }
  // Track the size of this uncommitted proposal.
  if (!increase_uncommitted_size(ents)) {
    LOG_DEBUG("%lu appending new entries to log would exceed uncommitted entry size limit; dropping proposal", id_);
    // Drop the proposal.
    return false;
  }

  // use latest "last" index after truncate/append
  li = raft_log_->append(ents);
  get_progress(id_)->maybe_update(li);
  // Regardless of maybeCommit's return, our caller will call bcastAppend.
  maybe_commit();
  return true;
}
/** Raft::tick_election() 是 Raft 协议中用于处理选举超时的函数。
 * 这个函数在 Raft 节点的 Candidate 或 Follower 状态中定期调用，用于检查是否应该启动新一轮的选举。 */
void Raft::tick_election() {
  election_elapsed_++;  // tick_election()函数每次调用都会增加，跟踪自上次以来经过的时间。
  // tick是每100ms出发的，所以这样记次数就是 elec_ela * 100 ms 即经过的时间。
// 如果可以成为leader且 past election timeout（elapse已经大于randomized的time out），开始选举
  if (promotable() && past_election_timeout()) { // promotable：是否可以成为领导，pastXX检查elect——ela是否已经超过阈值时间
    election_elapsed_ = 0; 
    proto::MessagePtr msg(new proto::Message());
    msg->from = id_;
    msg->type = proto::MsgHup;  // 这个节点决定开始新一轮的选举。自己准备成为leader。promotable是可以成为leader，而不是pre？
    step(std::move(msg));
  }
}

void Raft::tick_heartbeat() {
  heartbeat_elapsed_++;  // 这是一个计时器，用于记录自上次发送心跳消息以来经过的时间
  election_elapsed_++;  // 这是另一个计时器，用于记录自上次选举超时检查以来经过的时间。它通常在所有状态下都递增，

  if (election_elapsed_ >= election_timeout_) { // 如果前者大于后者，选举超时。需要重新选举
    election_elapsed_ = 0;
    if (check_quorum_) {  // 选举仲裁。发送MsgCheck Quorum消息，特殊的消息。用于确认当前leader是否仍然有多数支持
      proto::MessagePtr msg(new proto::Message());
      msg->from = id_;
      msg->type = proto::MsgCheckQuorum;
      step(std::move(msg));  // 消息通过step函数发送到raft算法的处理逻辑中。
    }
    // If current leader cannot transfer leadership in electionTimeout, it becomes leader again.
    if (state_ == RaftState::Leader && lead_transferee_ != 0) {
      abort_leader_transfer(); // 保留旧领导
    }
  }

  if (state_ != RaftState::Leader) {  // 如果当前节点不是leader，那么不执行后续heartbeat逻辑
    return;
  }
// elapsed是一个计时器变量，用于记录自上次发送心跳消息以来经过的时间。当elapsed超过timeout时
// 领导者需要发送一个新的心跳消息，并将elapsed其重置为0
  if (heartbeat_elapsed_ >= heartbeat_timeout_) { // 心跳超时，发送新的心跳。
    heartbeat_elapsed_ = 0;
    proto::MessagePtr msg(new proto::Message());
    msg->from = id_;
    msg->type = proto::MsgBeat;
    step(std::move(msg));
  }
}

bool Raft::past_election_timeout() {
  return election_elapsed_ >= randomized_election_timeout_;
}

void Raft::reset_randomized_election_timeout() {
  randomized_election_timeout_ = election_timeout_ + random_device_.gen();
  assert(randomized_election_timeout_ <= 2 * election_timeout_);
}

bool Raft::check_quorum_active() {
  size_t act = 0;
  for_each_progress([&act, this](uint64_t id, ProgressPtr& pr) {
    if (id == this->id_) {
      act++;
      return;
    }
    if (pr->recent_active && !pr->is_learner) {
      act++;
    }
  });

  return act >= quorum();
}

void Raft::send_timeout_now(uint64_t to) {
  proto::MessagePtr msg(new proto::Message());
  msg->to = to;
  msg->type = proto::MsgTimeoutNow;
  send(std::move(msg));
}

void Raft::abort_leader_transfer() {
  lead_transferee_ = 0;
}
// 增加uncommitted size：uncommitted_size_记录未提交日志条目总大小。entries是新增加的未提交的log
bool Raft::increase_uncommitted_size(const std::vector<proto::EntryPtr>& entries) {
  uint32_t s = 0;
  for (auto& entry : entries) {
    s += entry->payload_size();
  }
  // 限制uncommitted的日志数，上限为max uncommitted
  if (uncommitted_size_ > 0 && uncommitted_size_ + s > max_uncommitted_size_) {
    // If the uncommitted tail of the Raft log is empty, allow any size
    // proposal. Otherwise, limit the size of the uncommitted tail of the
    // log and drop any proposal that would push the size over the limit.
    return false;
  }
  uncommitted_size_ += s;
  return true;
}
// 降低uncommitted日志的数目。entries是一批已经提交的日志条目，需要从之前记录的uncommitted条目数中减去
void Raft::reduce_uncommitted_size(const std::vector<proto::EntryPtr>& entries) {
  if (uncommitted_size_ == 0) { // 如果为0，直接返回。这是一种优化路径，因为follower不需要跟踪或
  // 强制执行未提交日志条目的大小限制
    // Fast-path for followers, who do not track or enforce the limit.
    return;
  }

  uint32_t size = 0;

  for (const proto::EntryPtr& e: entries) {
    size += e->payload_size();  // 累加有效负载大小
  }
  if (size > uncommitted_size_) { // 如果计算出的日志条目大小size大于uncommitted，则设为0，防止溢出
    // uncommittedSize may underestimate the size of the uncommitted Raft
    // log tail but will never overestimate it. Saturate at 0 instead of
    // allowing overflow.
    uncommitted_size_ = 0;
  } else {
    uncommitted_size_ -= size;
  }
}

}