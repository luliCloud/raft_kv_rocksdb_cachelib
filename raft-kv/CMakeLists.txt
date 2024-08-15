
set(SRC
    common/log.h
    common/slice.h
    common/status.cpp
    common/bytebuffer.cpp
    common/random_device.cpp
    raft/proto.cpp
    raft/config.cpp
    raft/raft.cpp
    raft/storage.cpp
    raft/node.cpp
    raft/raft_log.cpp
    raft/unstable.cpp
    raft/progress.cpp
    raft/readonly.cpp
    raft/raft_status.cpp
    raft/ready.cpp
    raft/util.cpp
    server/raft_node.cpp
    server/redis_session.cpp
    server/redis_store.cpp
    snap/snapshotter.cpp
    transport/proto.h
    transport/transport.h
    transport/transport.cpp
    transport/peer.h
    transport/raft_server.cpp
    transport/peer.cpp
    wal/wal.cpp)
# create raft-kv++ lib
add_library(raft-kv++ ${SRC})
# ensure ROcksDB linked correctly
target_link_libraries(raft-kv++ ${LIBS} rocksdb_interface)

# create executable file raft-kv
add_executable(raft-kv raft-kv.cpp)
# linke raft-kv++ lib and RocksDB to executable file
target_link_libraries(raft-kv ${LIBS} raft-kv++ rocksdb_interface)

# RocksDB 链接库：
# 我们在主 CMakeLists.txt 文件中创建了 rocksdb_interface，它已经包含了 RocksDB 的链接信息。在这里，我们需要确保 rocksdb_interface 被正确链接到 raft-kv++ 和 raft-kv 目标。
# 链接顺序：在 target_link_libraries 中，rocksdb_interface 作为最后一个库被链接，这样可以确保链接器在处理 RocksDB 相关符号时能够正确解析。
# 完整性：通过在 raft-kv++ 和 raft-kv 目标中同时链接 LIBS 和 rocksdb_interface，确保了所有依赖库都能正确链接。


# lu modify since here
# 定义源文件列表
#set(COMMON
#    common/status.cpp
#    common/bytebuffer.cpp
#    common/random_device.cpp)

# 添加一个库
#add_library(common_lib STATIC ${COMMON})

# 为库设置私有包含目录
#target_include_directories(common_lib PRIVATE ${CMAKE_SOURCE_DIR}/common)
#message(STATUS "Including directory: ${CMAKE_SOURCE_DIR}/common")

