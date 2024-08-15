#include <gtest/gtest.h>
#include <raft-kv/common/bytebuffer.h>
/** 
这段测试代码是用来测试一个名为 ByteBuffer 的缓冲区类的功能。ByteBuffer 类提供了一些方法来存储、读取和操作字节数据。
通过以下几个操作和断言，这个测试验证了 ByteBuffer 的基本功能，如写入数据、读取数据、检查可读字节数等。 */
using namespace kv;

TEST(test_buffer, test_buffer) {
  ByteBuffer buff;

  buff.put((const uint8_t*) "abc", 3);
  ASSERT_TRUE(buff.readable());
  ASSERT_TRUE(buff.readable_bytes() == 3);

  buff.read_bytes(1);
  char buffer[4096] = {0};
  memcpy(buffer, buff.reader(), buff.readable_bytes());
  ASSERT_TRUE(buff.readable_bytes() == 2);
  ASSERT_TRUE(buffer == std::string("bc"));
  ASSERT_TRUE(buff.slice().to_string() == "bc");

  buff.read_bytes(2);
  ASSERT_TRUE(buff.readable_bytes() == 0);

  ASSERT_TRUE(buff.slice().to_string() == "");
  fprintf(stderr, "%d\n", buff.capacity());

  buff.put((const uint8_t*) "123456", 6);
  ASSERT_TRUE(buff.slice().to_string() == "123456");
}

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

