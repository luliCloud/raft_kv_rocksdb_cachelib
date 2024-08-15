#include <msgpack.hpp>  // need install
#include <gtest/gtest.h>
#include <raft-kv/raft/proto.h>

/** MSGPACK_DEFINE 是一个宏，用于简化在使用 MessagePack 序列化库时定义类的序列化和反序列化方法。
 * MessagePack 是一种高效的二进制序列化格式，用于将数据结构序列化成二进制流以及从二进制流反序列化
 * 成数据结构。在你的代码中，MSGPACK_DEFINE 宏用于定义 MyClass 类的序列化和反序列化行为。
 * 具体来说，这个宏会自动生成必要的代码，使得 MessagePack 库能够正确地处理 MyClass 对象的序列化
 * 和反序列化。
 */
class MyClass {
 public:
  std::string str;
  std::vector<int> vec;
 public:
  MSGPACK_DEFINE (str, vec);
};
/** 测试mypack的功能，将数据结构序列化成二进制流存储，以及从二进制流阀序列化成数据化结构 */
TEST(test_msgpack, test_msgpack) {
  std::vector<MyClass> vec;

  MyClass my;
  my.str = "abc";
  my.vec.push_back(1);
  my.vec.push_back(3);

  vec.push_back(std::move(my));  // vec is container of MyClass. see line 20

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, vec);  // 将vec转成二进制，放入sbuf
  /** 当你调用 msgpack::pack 函数时，MessagePack 库会检查传入对象是否有一个 msgpack_pack 方法。
   * 如果有，它就会调用这个方法来执行实际的序列化过程。 */

  msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size()); // decode sbuf

  msgpack::object obj = oh.get();
  std::vector<MyClass> rvec;
  obj.convert(rvec);  // 将obj中的内容放入rvec？

  ASSERT_TRUE(rvec.size() == 1);
  MyClass& out = rvec[0];  // rvec中就一个元素。因为就是之前定义的my
  ASSERT_TRUE(out.str == "abc");
  ASSERT_TRUE(out.vec.size() == 2);
  ASSERT_TRUE(out.vec[0] == 1);
  ASSERT_TRUE(out.vec[1] == 3);
}

TEST(test_msgpack, test_error) {
  std::vector<MyClass> vec;

  MyClass my;
  my.str = "abc";
  my.vec.push_back(1);
  my.vec.push_back(3);

  vec.push_back(std::move(my));

  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, vec);

  msgpack::object_handle oh = msgpack::unpack(sbuf.data(), sbuf.size());

  msgpack::object obj = oh.get();
  std::string out;
  ASSERT_ANY_THROW(obj.convert(out));  // 类型不匹配。obj应该是vec<MyClass>，没办法转成string out

}

class B {
 public:
  int a;
 public:
  MSGPACK_DEFINE (a); 
};

TEST(msgpack, entry_size) {
  using namespace kv::proto;

  Entry entry;
  entry.type = 10;
  entry.term = 10;
  entry.index = 10;
  {
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.type = 255;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.term = std::numeric_limits<uint8_t>::max() - 10;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }
// 测试在unit8-t，16-t，32-t的情况下都能work？平台兼容性。8:0-255； 16:0-65535； 32：0-4294967295
// 每种类型的数据都需要不同的字节数表示。因此序列化库必须能够正确处理这些差异
  {
    entry.term = std::numeric_limits<uint8_t>::max() + 10;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.term = std::numeric_limits<uint16_t>::max() + 10;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.term = std::numeric_limits<uint32_t>::max() + 10;
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.data.resize(std::numeric_limits<uint8_t>::max() - 1);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.data.resize(std::numeric_limits<uint16_t>::max() - 1);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_TRUE(entry.serialize_size() == sbuf.size());
  }

  {
    entry.data.resize(std::numeric_limits<uint16_t>::max() + 1);
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, entry);
    ASSERT_EQ(entry.serialize_size(), sbuf.size());
  }
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}