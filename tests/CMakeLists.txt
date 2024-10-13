set(CMAKE_INSTALL_RPATH "/home/lu/tools/CacheLib/opt/cachelib/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# 指定所有 CacheLib 依赖的库路径
set(CACHELIB_DEPENDENCIES_DIR "/home/lu/tools/CacheLib/opt/cachelib")

# 设置动态库搜索路径，确保使用 CacheLib 中的库
link_directories(${CACHELIB_DEPENDENCIES_DIR}/lib)

# 添加 CacheLib 依赖库的头文件路径
include_directories(
    "${CACHELIB_DEPENDENCIES_DIR}/include/folly"
    "${CACHELIB_DEPENDENCIES_DIR}/include/glog"
    "${CACHELIB_DEPENDENCIES_DIR}/include/gflags"
    "${CACHELIB_DEPENDENCIES_DIR}/include/fmt"
    "${CACHELIB_DEPENDENCIES_DIR}/include/wangle"
    "${CACHELIB_DEPENDENCIES_DIR}/include/fizz"
    "${CACHELIB_DEPENDENCIES_DIR}/include/mvfst"
    "${CACHELIB_DEPENDENCIES_DIR}/include/fbthrift"
    "${CACHELIB_DEPENDENCIES_DIR}/include/sparse-map"
)
set(CACHELIB_DEPENDENCIES_LIBS
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_client.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_server.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_fizz_client.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_fizz_handshake.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_loss.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libmvfst_flowcontrol.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libfmt.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libfolly.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libgflags.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libglog.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libwangle.so"
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libfizz.so"
    
    # 添加 fbthrift 相关的库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthriftcpp2.so"           # Thrift C++2 库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthriftprotocol.so"        # Thrift 协议库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthriftmetadata.so"        # Thrift 元数据库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthriftfrozen2.so"         # Thrift Frozen 协议库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthrift-core.so"           # Thrift 核心库
    "${CACHELIB_DEPENDENCIES_DIR}/lib/libthriftannotation.so"      # Thrift 注解库
)

# 将 CacheLib 和它的依赖库加入到 LIBS 变量中
set(LIBS
    ${LIBS}
    ${CACHELIB_LIBRARIES}  # CacheLib 静态库
    ${CACHELIB_DEPENDENCIES_LIBS} # CacheLib 的依赖库
    raft-kv++             # 你自己的库
)


enable_testing()
find_package(GTest REQUIRED)
# 添加测试可执行文件和库链接
add_executable(test_msgpack test_msgpack.cpp)
target_link_libraries(test_msgpack ${LIBS})
gtest_add_tests(TARGET test_msgpack)

add_executable(test_bytebuffer test_bytebuffer.cpp)
target_link_libraries(test_bytebuffer ${LIBS})
gtest_add_tests(TARGET test_bytebuffer)

add_executable(test_proto test_proto.cpp)
target_link_libraries(test_proto ${LIBS})
gtest_add_tests(TARGET test_proto)

add_executable(test_storage test_storage.cpp)
target_link_libraries(test_storage ${LIBS})
gtest_add_tests(TARGET test_storage)

add_executable(test_unstable test_unstable.cpp)
target_link_libraries(test_unstable ${LIBS})
gtest_add_tests(TARGET test_unstable)

add_executable(test_raftlog test_raftlog.cpp)
target_link_libraries(test_raftlog ${LIBS})
gtest_add_tests(TARGET test_raftlog)

add_executable(test_progress test_progress.cpp)
target_link_libraries(test_progress ${LIBS})
gtest_add_tests(TARGET test_progress)

add_executable(test_raft test_raft.cpp network.hpp)
target_link_libraries(test_raft ${LIBS})
gtest_add_tests(TARGET test_raft)

add_executable(test_rawnode test_rawnode.cpp)
target_link_libraries(test_rawnode ${LIBS})
gtest_add_tests(TARGET test_rawnode)

add_executable(string_match string_match.cpp)
target_link_libraries(string_match ${LIBS})
gtest_add_tests(TARGET string_match)

add_executable(raft_snap_test raft_snap_test.cpp network.hpp)
target_link_libraries(raft_snap_test ${LIBS})
gtest_add_tests(TARGET raft_snap_test)

add_executable(test_snapshotter test_snapshotter.cpp)
target_link_libraries(test_snapshotter ${LIBS})
gtest_add_tests(TARGET test_snapshotter)

add_executable(test_wal test_wal.cpp)
target_link_libraries(test_wal ${LIBS})
gtest_add_tests(TARGET test_wal)

# for gtest and test_resdis_store_rocksdb
add_executable(test_kvstore_rocksdb test_kvstore_rocksdb.cpp)
# 将目标 test_kvstore_map 与库 LIBS 进行链接，确保它在编译时能找到并使用所需的库。
target_link_libraries(test_kvstore_rocksdb ${LIBS}) 
# 从 test_raftlog 可执行文件中提取测试用例，并将其添加到 CMake 的测试框架中。
# CMake 会自动查找使用 Google Test 
# 框架编写的测试函数（例如 TEST() 宏定义的测试），并将这些测试用例注册到 ctest 测试工具中。
gtest_add_tests(TARGET test_kvstore_rocksdb)