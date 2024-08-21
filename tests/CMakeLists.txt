
set(LIBS
    ${LIBS}
    raft-kv++)

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

# for gtest and test_resdis_store
add_executable(test_kvstore_map test_kvstore_map.cpp)
# 将目标 test_kvstore_map 与库 LIBS 进行链接，确保它在编译时能找到并使用所需的库。
target_link_libraries(test_kvstore_map ${LIBS}) 
# 从 test_raftlog 可执行文件中提取测试用例，并将其添加到 CMake 的测试框架中。
# CMake 会自动查找使用 Google Test 
# 框架编写的测试函数（例如 TEST() 宏定义的测试），并将这些测试用例注册到 ctest 测试工具中。
gtest_add_tests(TARGET test_kvstore_map)

# for gtest and test_resdis_store_rocksdb
add_executable(test_kvstore_rocksdb test_kvstore_rocksdb.cpp)
target_link_libraries(test_kvstore_rocksdb ${LIBS}) 
gtest_add_tests(TARGET test_kvstore_rocksdb)