#include <cstdio>
#include "raft-kv/raft/proto.h"
using namespace kv;

int main(int argc, char* argv[]) {
  for (int i = 0; i <= proto::MsgPreVoteResp; ++i) {
    const char* str = proto::msg_type_to_string(i);
    fprintf(stderr, " %d, %s\n", i, str);
  }

  for (int i = 0; i < 2; ++i) {
    const char* str = proto::entry_type_to_string(i);
    fprintf(stderr, " %d, %s\n", i, str);
  }
}
/**
 * // msg_type 
 0, MsgHup
 1, MsgBeat
 2, MsgProp
 3, MsgApp
 4, MsgAppResp
 5, MsgVote
 6, MsgVoteResp
 7, MsgSnap
 8, MsgHeartbeat
 9, MsgHeartbeatResp
 10, MsgUnreachable
 11, MsgSnapStatus
 12, MsgCheckQuorum
 13, MsgTransferLeader
 14, MsgTimeoutNow
 15, MsgReadIndex
 16, MsgReadIndexResp
 17, MsgPreVote
 18, MsgPreVoteResp
 // entry type:
 0, EntryNormal
 1, EntryConfChange
 */