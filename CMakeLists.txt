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