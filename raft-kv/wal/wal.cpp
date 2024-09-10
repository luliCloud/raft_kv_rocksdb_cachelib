#include <raft-kv/wal/wal.h>
#include <raft-kv/common/log.h>
#include <boost/filesystem.hpp> // C++17 and boost both have filesystem lib. we use boost here
#include <fcntl.h>
#include <raft-kv/raft/util.h>
#include <sstream>
#include <inttypes.h>
#include <raft-kv/raft/util.h>

namespace kv {
// 定义了所有WAL type 类型。
static const WAL_type wal_InvalidType = 0; // 表示无效的日志记录。通常用于初始化或表示某种错误
static const WAL_type wal_EntryType = 1; // 普通的日志条目。日志条目包含了实际的简直对数据
static const WAL_type wal_StateType = 2; // 该日志条目表示数据库的某种状态信息，，如HardState。状态纪录包含了系统的元数据，通常用于纪录系统的状态变化，如选举信息，节点信息等
static const WAL_type wal_CrcType = 3; // 用于校验的数据记录，CRC
static const WAL_type wal_snapshot_Type = 4; // 这个日志是一个快照记录。包含了数据库在某个时间点的完整状态，通常用于恢复操作。
static const int SegmentSizeBytes = 64 * 1000 * 1000; // 64MB

/**
 * 这个函数的作用是生成一个符合特定格式的 WAL（Write-Ahead Log） 文件名。
 * 它通过将两个 64 位无符号整数（seq 和 index）格式化为一个字符串
 * 并将其作为文件名的一部分。seq是日志文件的顺序号，保证日志文件按时间排列且唯一。index是日志条目的序列号
 * 保证日志条目的顺序性和唯一性，是日志条目的唯一索引，可以迅速定位日志。一个日志文件包含多个日志条目。
 */
static std::string wal_name(uint64_t seq, uint64_t index) {
  char buffer[64];
  // 式化字符串：使用 snprintf 函数将 seq 和 index 格式化为一个字符串。
  // 格式化后的字符串的格式为 "{seq}-{index}.wal"，其中 seq 和 index 都是 16 位的十六进制数（使用 "%016" PRIx64 格式化）。
  // 这种格式确保生成的文件名具有固定的长度和特定的格式，便于排序和解析。
  snprintf(buffer, sizeof(buffer), "%016" PRIx64 "-%016" PRIx64 ".wal", seq, index);
  return buffer;
}

/**
WAL_File 类在键值存储系统中管理预写日志（Write-Ahead Log，简称 WAL）的操作。
该类提供了创建、截断、追加和读取 WAL 文件的功能。同时实现原子性操作。
 */
class WAL_File {
 public:
  WAL_File(const char* path, int64_t seq)
      : seq(seq),
        file_size(0) {
    // 使用fopen函数以追加和读写模式（a+）打开指定路径文件。如果不存在，则创建。如果文件存在，fp指向末尾
    fp = fopen(path, "a+");
    if (!fp) {
      LOG_FATAL("fopen error %s", strerror(errno));
    }

    file_size = ftell(fp);
    if (file_size == -1) {
      LOG_FATAL("ftell error %s", strerror(errno));
    }

    if (fseek(fp, 0L, SEEK_SET) == -1) {
      LOG_FATAL("fseek error %s", strerror(errno));
    }
  }

  ~WAL_File() {
    fclose(fp);
  }

/** 这个function可能的作用
 * 日志轮换（Log Rotation）：是一种常见的日志管理策略，定期将当前日志文件重命名或移动，
 * 并开始记录到一个新的日志文件。旧日志文件可以根据策略进行压缩、归档或删除。
 */
  void truncate(size_t offset) { // offset在这里起的作用应该是让我们从文件中的offset位置开始读
  // 可能offset位置是一个data entry的位置。
    // ftruncate(fileno(fp), offset)：将文件描述符 fp 指向的文件截断或扩展到 offset 字节大小。
    // 如果 ftruncate 函数返回非零值，则表示截断失败，调用 LOG_FATAL 记录致命错误并输出具体错误信息。
    if (ftruncate(fileno(fp), offset) != 0) {
      LOG_FATAL("ftruncate error %s", strerror(errno));
    }
    // 让fp 指针移动到offset的位置
    if (fseek(fp, offset, SEEK_SET) == -1) {
      LOG_FATAL("fseek error %s", strerror(errno));
    }
    // 我们在这里将原始文件一分为二，并将截断的前部分定义为offset大小。fp则使得下次我们会从新文件开始读
    file_size = offset; // file_size define 定义在最后
    data_buffer.clear();
  }
/** 将新的data append到当前data_buffer(缓冲期)的最后 */
  void append(WAL_type type, const uint8_t* data, size_t len) {
    WAL_Record record;
    record.type = type;
    record.crc = compute_crc32((char*) data, len);
    set_WAL_Record_len(record, len);
    uint8_t* ptr = (uint8_t*) &record; // 强制转换，数据类型可能本来不是uint8_t
    // 把data的type和crc放在data的前面。crc用于检查data的内容和完整性
    data_buffer.insert(data_buffer.end(), ptr, ptr + sizeof(record));
    data_buffer.insert(data_buffer.end(), data, data + len);
  }
/** 将data_buffer中的数据写到日志文件中 */
  void sync() {
    if (data_buffer.empty()) {
      return;
    }
// fp是在当前日志文件中的最后位置。这里使用fwirte将缓冲区中的数据写到fp指向的文件中。
// data_buffer.data()指向要写入数据的指针，1:每次写入1 byte；size：要写入元素的数量。fp：目标文件
    size_t bytes = fwrite(data_buffer.data(), 1, data_buffer.size(), fp);
    if (bytes != data_buffer.size()) {
      LOG_FATAL("fwrite error %s", strerror(errno));
    }

    file_size += data_buffer.size(); // update file size
    data_buffer.clear();
  }
/** 将fp指向的文件中的所有内容读到out中 */
  void read_all(std::vector<char>& out) {
    char buffer[1024];
    while (true) {
      /** 使用 fread 函数从文件中读取数据，每次读取最多 1024 字节，并将读取到的字节数存储在 bytes 变量中。
       * 在 read_all 函数中，缓冲区 buffer 读完后不需要显式清空，因为每次读取操作会覆盖缓冲区中的旧数据。
       */
      size_t bytes = fread(buffer, 1, sizeof(buffer), fp);
      if (bytes <= 0) {
        break;
      }
      /** 将buffer中的前bytes字节存储到out中 */
      out.insert(out.end(), buffer, buffer + bytes);
    }

    fseek(fp, 0L, SEEK_END); // 将fp移到已读文件的末尾
  }

  std::vector<uint8_t> data_buffer;
  int64_t seq;
  long file_size;
  FILE* fp;
};

void WAL::create(const std::string& dir) {
  using namespace boost;
  // ‘/’用于连接两个路径部分。wal_name 是在该文件最上面定义的。创建一个WAL日志文件。以seq和idx 命名
  filesystem::path walFile = filesystem::path(dir) / wal_name(0, 0);
  std::string tmpPath = walFile.string() + ".tmp";

  if (filesystem::exists(tmpPath)) {
    filesystem::remove(tmpPath);
  }
// {}用来在离开作用域时自动释放share_ptr
// 这里的WAL是系统初始化时第一次创建日志的基准snapshot，所以为0，0.在系统off前如果我们创建第二个日志
// 文件，要加载的snapshot是从当前状态的term 和 index开始的。
// 这段代码的主要作用是创建一个新的 WAL（Write-Ahead Logging）文件，将一个 WAL_Snapshot 
// 对象序列化为二进制数据，并将其追加到 WAL 文件中，然后同步数据。
  {
    std::shared_ptr<WAL_File> wal(new WAL_File(tmpPath.c_str(), 0));
    WAL_Snapshot snap; // WAL_snapshot 定义在wal.h 有任期和日志的idx，用来确定日志的唯一性和选举？
    snap.term = 0;
    snap.index = 0; // snap.index 是对应WAL的index还是seq？感觉是日志条目的index
    msgpack::sbuffer sbuf; // MsgPack（MessagePack）是一个高效的二进制序列化格式库，它允许将复杂的数据结构序列化为紧凑的二进制表示
    msgpack::pack(sbuf, snap); // 将 snap 对象序列化为二进制格式，并将序列化后的数据存储在 sbuf 中。
    // wal是前面创造的share ptr。这里将sbuf中的内容存到tmp Path里
    wal->append(wal_snapshot_Type, (uint8_t*) sbuf.data(), sbuf.size());
    wal->sync();  // 同步数据到硬盘，使日志永久化
  }
// walFile 是这个create函数最早开始声明的一个路径。我们在tmp中生成了WAL——FILE，然后转移到真正存walFile的地方
  filesystem::rename(tmpPath, walFile);
  /**
   * 在临时路径（tmpPath）中生成 WAL 文件，然后再转移到目标路径（walFile），
   * 这种操作通常是为了确保数据的完整性和一致性。以下是一些常见的原因和好处：
原子性操作：文件系统的重命名操作通常是原子的（atomic），这意味着操作要么完全成功，要么完全失败，
不会有中间状态。因此，通过在临时路径中生成文件，然后使用 rename 函数移动到目标路径，
可以确保文件的完整性和一致性。
   */
}
/**
 * 实际上，通常的流程是将所有写操作首先写入一个临时文件中，当所有写操作完成并且数据同步到存储设备后，
 * 再将临时文件原子性地重命名或移动到目标文件路径。
 * 这确保了文件在整个写操作过程中始终保持一致性和完整性。
在后续的操作中，如果需要追加新的日志条目到已经存在的 WAL 文件中，
通常不再需要每次都创建一个新的临时文件再移动，而是直接在现有的 WAL 文件中进行追加操作并同步。
这是因为新日志条目的追加操作通常不涉及覆盖或替换整个文件的内容，而只是简单地在文件末尾添加新数据。
 */

/**
 * 这个函数 WAL::open 的主要目的是打开一个现有的 Write-Ahead Log (WAL) 并进行一系列验证操作，
 * 以确保日志文件的有效性和正确顺序。ptr指向一个WAL。dir是WAL日志文件所在的目录。snap是日志的起始快照
 */
WAL_ptr WAL::open(const std::string& dir, const WAL_Snapshot& snap) {
  LOG_INFO("loading WAL at term %lu and index %lu in WALL::open()", snap.term, snap.index);  // lu: debug
  WAL_ptr w(new WAL(dir)); // 创建一个新的WAL，并让WAL ptr指向它。WAL管理一个dir的WAL文件。而不是指single WAL

  std::vector<std::string> names;
  w->get_wal_names(dir, names); // 获取dir下所有WAL文件名
  if (names.empty()) {
    LOG_FATAL("wal not found");
  }

  uint64_t nameIndex;
  // 搜索与snap index匹配的WAL文件。snap存的是一个WAL的起始快照信息，
  // 所以通常snap.index也与WAL的起始日志条目的index一致
  if (!WAL::search_index(names, snap.index, &nameIndex)) { //&nameIndex存放是该names vector中第几个文件
    LOG_FATAL("wal not found");
  }
// 从找到的匹配文件开始，检查所有后续WAL文件的序列是否有效
  std::vector<std::string> check_names(names.begin() + nameIndex, names.end());
  if (!WAL::is_valid_seq(check_names)) {
    LOG_FATAL("invalid wal seq");
  }
// iterate所有验证后的WAL，解析他们的index和seq。将path和seq创建出的新的
  for (const std::string& name: check_names) {
    uint64_t seq;
    uint64_t index;
    if (!parse_wal_name(name, &seq, &index)) {
      LOG_FATAL("invalid wal name %s", name.c_str());
    }
    // 注意这里的new WAL_FILE 是日志的文件句柄！而不是文件本身。查看WAL_FILE constructor
    // 它是用open函数的打开了指定路径的文件。所以我们可以用这个作为文件句柄去管理这个
    // WAL FILE包括调用相关函数。而不只是简单的读取和write
    boost::filesystem::path path = boost::filesystem::path(w->dir_) / name;
    std::shared_ptr<WAL_File> file(new WAL_File(path.string().c_str(), seq));
    w->files_.push_back(file);
  }

  memcpy(&w->start_, &snap, sizeof(snap)); // 将指定的快照snap复制到WAL对象的start_成员中
  return w;
}

/**
 * proto 通常指的是 Protocol Buffers（protobuf），它是由 Google 开发的一种语言中立、平台中立、
 * 可扩展的序列化结构数据的方法。
 * Protocol Buffers 本身并不是一个库，而是一种接口描述语言（IDL）和用于序列化数据的工具。
 * 
 * 该函数读取所有WAL文件的内容，解析并验证每个日志记录，
 * 返回一个Status类型，接受两个参数hs和ents，用于存储解析后的HardState和日志条目
 * 
 * 这个函数主要是为了系统恢复或重启时，将WAL文件中的数据以恢复HardState和日志条目（ents是日志ptr的集合）
 */
Status WAL::read_all(proto::HardState& hs, std::vector<proto::EntryPtr>& ents) {
  std::vector<char> data;
  for (auto file : files_) { // 处理每一个文件
    data.clear();
    file->read_all(data); // 将文件中的内容读到data中
    size_t offset = 0;  // 用于跟踪读取的位置
    bool matchsnap = false; // 标记是否找到快照记录

    while (offset < data.size()) {
      size_t left = data.size() - offset; // 计算剩余未处理的数据长度 left
      size_t record_begin_offset = offset; // 记录这个循环从哪里开始读起

      if (left < sizeof(WAL_Record)) { // 如果剩余数据 不足一个WAL-record 截断文件并警告 
      //（只有WAL-record 没有data是可以的。null）
        file->truncate(record_begin_offset);
        LOG_WARN("invalid record len %lu", left);
        break;
      }

      WAL_Record record; // 注意WAL_record 不是data部分，而是该纪录的类型，数据长度，和预期的CRC
      // data.data()指向了data的第一个char。offset就是目前在data中的偏移量（读到了data的第几个char）
      // 所以sizeof就是让我们从offset开始读一个记录的长度到record里
      memcpy(&record, data.data() + offset, sizeof(record));

      left -= sizeof(record);
      offset += sizeof(record); // 偏移量根据record长度自动后移了

      if (record.type == wal_InvalidType) {
        break;
      }

      uint32_t record_data_len = WAL_Record_len(record); // 从record中判断数据的长度吗？
      if (left < record_data_len) {  // 如果剩余数据不足以容纳下一个WAL_record
      // (这个日志条目没有数据下一个也没有)，说明下一个record残缺
        file->truncate(record_begin_offset);
        LOG_WARN("invalid record data len %lu, %u", left, record_data_len);
        break;
      }

      char* data_ptr = data.data() + offset; // data ptr指向了
      // 计算数据部分的校验和，并与record.crc 比较
      uint32_t crc = compute_crc32(data_ptr, record_data_len);

      left -= record_data_len;
      offset += record_data_len;

      if (record.crc != 0 && crc != record.crc) {
        file->truncate(record_begin_offset);
        LOG_WARN("invalid record crc %u, %u", record.crc, crc);
        break;
      }
// 处理记录 根据记录类型（普通数据，快照，hardstate。。。）决定如何处理数据
      handle_record_wal_record(record.type, data_ptr, record_data_len, matchsnap, hs, ents);
// 如果记录类型为快照，将match snap设置为true
      if (record.type == wal_snapshot_Type) {
        matchsnap = true;
      }
    }

    if (!matchsnap) { // 读完所有的文件后 如果都没有找到快照，说明系统无法恢复。这里已经跳出读所有file的loop
      LOG_FATAL("wal: snapshot not found");
    }
  }

  return Status::ok();
}
/**
 * WAL_type type" 纪录的类型，可以是日志条目，状态，快照或其他类型。完整定义在该文件开头
 */
void WAL::handle_record_wal_record(WAL_type type,
                                   const char* data,
                                   size_t data_len,
                                   bool& matchsnap,
                                   proto::HardState& hs,
                                   std::vector<proto::EntryPtr>& ents) {

  switch (type) {
    case wal_EntryType: { // 普通日志条目
      proto::EntryPtr entry(new proto::Entry()); // 创建一个新的日志条目对象entry。entry定义在proto.cpp
      msgpack::object_handle oh = msgpack::unpack(data, data_len); // 反序列化数据到entry
      oh.get().convert(*entry);
// 如果条目的索引大雨start_的索引，调整ents的大小并将entry添加到ents中
      if (entry->index > start_.index) {
        ents.resize(entry->index - start_.index - 1);
        ents.push_back(entry);
      }

      enti_ = entry->index; // enti_: index of the last entry saved to the wal. defind in .h
      break;
    }

    case wal_StateType: { // 反序列化数据到“hardstate”
      msgpack::object_handle oh = msgpack::unpack(data, data_len);
      oh.get().convert(hs);
      break;
    }

    case wal_snapshot_Type: {
      WAL_Snapshot snap;
      msgpack::object_handle oh = msgpack::unpack(data, data_len);
      oh.get().convert(snap);

      if (snap.index == start_.index) { // start_就是系统恢复时起始的那个snapshot
        if (snap.term != start_.term) {
          LOG_FATAL("wal: snapshot mismatch");
        }
        matchsnap = true; // 找到了这个snapshot
      }
      break;
    }

    case wal_CrcType: { // 在一般的日志记录和恢复系统中，CRC（循环冗余校验）类型的记录用于校验数据的完整性，而不是作为独立的记录类型来存储实际的数据操作。
      LOG_FATAL("wal crc type");
      break;
    }
    default: {
      LOG_FATAL("invalid record type %d", type);
    }
  }
}
// 保存传入的hardstate和日志条目。根据需要决定是否进行同步以及切分日志文件
Status WAL::save(proto::HardState hs, const std::vector<proto::EntryPtr>& ents) {
  // short cut, do not call sync
  if (hs.is_empty_state() && ents.empty()) {
    return Status::ok();
  }

  bool mustSync = is_must_sync(hs, state_, ents.size());
  Status status;

  for (const proto::EntryPtr& entry: ents) { // 循环所有的entry
    status = save_entry(*entry);
    if (!status.is_ok()) {
      return status;
    }
  }

  status = save_hard_state(hs);
  if (!status.is_ok()) {
    return status;
  }
// 如果当前文件大小小于设定的段大小（SegmentSizeBytes），并且必须同步，则同步当前文件并返回成功状态。
// 否则，调用 cut 函数切分日志文件。
  if (files_.back()->file_size < SegmentSizeBytes) {
    if (mustSync) {
      files_.back()->sync();
    }
    return Status::ok();
  }

  return cut();
}
// 这个函数负责切分当前的日志文件。
// 在当前日志文件达到一定大小时，调用此函数将其同步，然后创建新的日志文件。
Status WAL::cut() {
  files_.back()->sync(); // 通过sync将buffer中的数据写到日志中
  return Status::ok();
}

Status WAL::save_snapshot(const WAL_Snapshot& snap) {
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, snap); // 序列话snap到sbuf中

  files_.back()->append(wal_snapshot_Type, (uint8_t*)sbuf.data(), sbuf.size());
  if (enti_ < snap.index) { // 更新enti_到当前idx
    enti_ = snap.index;
  }
  files_.back()->sync();  // files.back() 中files就是vector<WAL_File>. 所以就是最后一个也就是
  //最新的日志文件（注意一个日志文件中应该有多个log）.那么我们对这个WAL_File 进行sync

  return Status::ok();
}

Status WAL::save_entry(const proto::Entry& entry) {
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, entry);

  files_.back()->append(wal_EntryType, (uint8_t*)sbuf.data(), sbuf.size());
  enti_ = entry.index;
  return Status::ok();
}

Status WAL::save_hard_state(const proto::HardState& hs) {
  if (hs.is_empty_state()) {
    return Status::ok();
  }
  state_ = hs;

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, hs);
  files_.back()->append(wal_StateType, (uint8_t*)sbuf.data(), sbuf.size());
  return Status::ok();
}

void WAL::get_wal_names(const std::string& dir, std::vector<std::string>& names) {
  using namespace boost;

  filesystem::directory_iterator end;
  for (boost::filesystem::directory_iterator it(dir); it != end; it++) {
    filesystem::path filename = (*it).path().filename();
    filesystem::path extension = filename.extension();
    if (extension != ".wal") {
      continue;
    }
    names.push_back(filename.string());
  }
  std::sort(names.begin(), names.end(), std::less<std::string>());
}
// 占位函数，暂时返回成功状态
Status WAL::release_to(uint64_t index) {
  return Status::ok();
}
// 解析WAL FILE名称，提取seq和index
bool WAL::parse_wal_name(const std::string& name, uint64_t* seq, uint64_t* index) {
  *seq = 0;
  *index = 0;

  boost::filesystem::path path(name);
  if (path.extension() != ".wal") {
    return false;
  }

  std::string filename = name.substr(0, name.size() - 4); // trim ".wal"
  size_t pos = filename.find('-'); // - 分隔seq和index
  if (pos == std::string::npos) {
    return false;
  }

  try {
    {
      std::string str = filename.substr(0, pos);
      std::stringstream ss;
      ss << std::hex << str;
      ss >> *seq;
    }

    {
      if (pos == filename.size() - 1) {
        return false;
      }
      std::string str = filename.substr(pos + 1, filename.size() - pos - 1);
      std::stringstream ss;
      ss << std::hex << str;
      ss >> *index;
    }
  } catch (...) {
    return false;
  }
  return true;
}

bool WAL::is_valid_seq(const std::vector<std::string>& names) {
  uint64_t lastSeq = 0;
  for (const std::string& name: names) {
    uint64_t curSeq; // 用于跟踪上一个文件的序列号
    uint64_t i;
    if (!WAL::parse_wal_name(name, &curSeq, &i)) {
      LOG_FATAL("parse correct name should never fail %s", name.c_str());
    }

    if (lastSeq != 0 && lastSeq != curSeq - 1) { // 检查lastseq是否为0，如果不是，并且不比前一个大1.
    // 序列号不连续
      return false;
    }
    lastSeq = curSeq;
  }
  return true;
}

// searchIndex returns the last array index of names whose raft index section is
// equal to or smaller than the given index.
// The given names MUST be sorted.
bool WAL::search_index(const std::vector<std::string>& names, uint64_t index, uint64_t* name_index) {

  for (size_t i = names.size() - 1; i >= 0; --i) {
    const std::string& name = names[i];
    uint64_t seq;
    uint64_t curIndex;
    if (!parse_wal_name(name, &seq, &curIndex)) {
      LOG_FATAL("invalid wal name %s", name.c_str());
    }

    if (index >= curIndex) {
      *name_index = i;
      return true;
    }
    if (i == 0) {
      break;
    }
  }
  *name_index = -1;
  return false;
}

}
/**
 * WAL_File 类： 在目录 dir 中保存一个 WAL 文件实际上意味着该目录中存在一个具体的 WAL 文件，
 * 而每个 WAL 文件在代码中会被表示为一个 WAL_File 实例。这个 
 * WAL_File 实例封装了对这个文件的各种操作（如读、写、同步等）以及文件的元数据（如路径、序列号等）。
WAL_File 类通常表示一个具体的写前日志（Write-Ahead Logging，WAL）文件。
它封装了对单个 WAL 文件的操作和管理，包括文件的打开、写入、同步等操作。
该类主要负责具体的文件操作，处理单个日志文件的读写。
主要职责：
打开或创建一个 WAL 文件。
写入日志条目到文件。
同步文件内容到磁盘。
管理文件的元数据，例如序列号和索引。

WAL 类
WAL 类通常表示整个写前日志系统的管理器或抽象层。它负责管理多个 WAL_File 实例，
并提供对外的接口来管理整个日志系统。该类处理更高级别的操作，例如打开日志系统、验证日志文件序列、
追加新日志条目等。
主要职责：
管理多个 WAL_File 实例。
提供打开、关闭和管理日志系统的接口。
确保日志文件的顺序和一致性。
处理日志文件的滚动和归档。

 */
