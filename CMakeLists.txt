cmake_minimum_required(VERSION 3.5)
project(raft-kv)

set(CMAKE_CXX_STANDARD 17) # for rocksdb

add_definitions(-Wall)
add_definitions(-DDEBUG)

include_directories(${CMAKE_SOURCE_DIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(dependencies
                  glib-2.0>=2.10 REQUIRED
                  msgpack REQUIRED
                  hiredis REQUIRED)
include_directories(${dependencies_INCLUDE_DIRS})
set(LIBS
    ${dependencies_LIBRARIES})

find_package(Boost REQUIRED COMPONENTS system filesystem)
include_directories(${Boost_INCLUDE_DIRS})
set(LIBS
    ${LIBS}
    pthread
    Boost::system
    Boost::filesystem
    )

# for rocksdb link
find_package(RocksDB REQUIRED)
# add RocksDB INterface
# 这一行代码创建了一个名为 rocksdb_interface 的 INTERFACE 库。INTERFACE 库在 CMake 中是一种特殊的库类型，用于在没有生成实际的静态或动态库文件的情况下，在目标之间共享包含路径、编译选项和链接库。
# INTERFACE 库的作用：它不会生成物理上的库文件，而是提供了一种机制，让你可以在多个目标中共享编译和链接的选项。它特别适用于像 RocksDB 这样需要在多个目标中共享链接选项的情况。
add_library(rocksdb_interface INTERFACE)
# 将RocksDB::rocksdb 链接 rocksdb_interface
# INTERFACE 选项：表示链接的是 INTERFACE 库，这意味着任何链接到 rocksdb_interface 的目标都会自动继承 RocksDB::rocksdb 的链接设置。
target_link_libraries(rocksdb_interface INTERFACE RocksDB::rocksdb)

# 设置 LD_LIBRARY_PATH
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

# 添加 CacheLib 及其依赖库
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


# 添加 CacheLib 头文件路径
include_directories("/home/lu/tools/CacheLib/opt/cachelib/include")

# 添加 CacheLib 的库文件路径
# 链接 CacheLib 的静态库，明确指定每个库文件的路径
set(CACHELIB_LIBRARIES
    "/home/lu/tools/CacheLib/opt/cachelib/lib/libcachelib_allocator.a"
    "/home/lu/tools/CacheLib/opt/cachelib/lib/libcachelib_common.a"
    "/home/lu/tools/CacheLib/opt/cachelib/lib/libcachelib_datatype.a"
    "/home/lu/tools/CacheLib/opt/cachelib/lib/libcachelib_navy.a"
    "/home/lu/tools/CacheLib/opt/cachelib/lib/libcachelib_shm.a"
)

# 链接 CacheLib 库和其他依赖库
set(LIBS
    ${LIBS}
    ${CACHELIB_LIBRARIES}  # CacheLib 库
    ${CACHELIB_DEPENDENCIES_LIBS}
)

# # 查找 cachebench 可执行文件
# set(CACHELIB_BIN_DIR "/home/lu/tools/CacheLib/opt/cachelib/bin")
# find_program(CACHEBENCH_EXECUTABLE cachebench HINTS ${CACHELIB_BIN_DIR})
# message(STATUS "Found cachebench: ${CACHEBENCH_EXECUTABLE}")


add_subdirectory(raft-kv)
# for test。可以添加两个子目录, 注意已经在google test那里添加了。在这个文件里。不要重复！！！
# add_subdirectory(tests) 

pkg_check_modules(GoogleTest gtest_main)
if (GoogleTest_FOUND)
    include_directories(${GoogleTest_INCLUDE_DIRS})
    set(LIBS
        ${LIBS}
        ${GoogleTest_LIBRARIES})
    add_subdirectory(tests)
    enable_testing()
endif (GoogleTest_FOUND)

configure_file(Procfile.in Procfile)

# check root dir
message(STATUS "Including root directory: ${CMAKE_SOURCE_DIR}")

# # 自定义目标：单独运行 cachebench
# add_custom_target(run_cachebench
#     COMMAND ${CACHEBENCH_EXECUTABLE} --json_test_config /path/to/config.json
#     COMMENT "Running cachebench with JSON configuration"
# )