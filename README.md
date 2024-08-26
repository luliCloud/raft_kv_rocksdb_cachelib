# Raft-KV Product

This product provides robust key-value storage tailored for distributed database systems, engineered to meet the demands of applications requiring **high availability**, **data consistency**, and exceptional **fault tolerance**, even in network partition scenarios. It’s an ideal solution for industries like **finance**, **e-commerce**, and **social media**.

Our stress tests demonstrated impressive performance, with 4500 QPS for 4KB KV writes and 26000 QPS for 5KB KV reads, all while maintaining a P99 latency of less than 40 milliseconds. The performance across different size of QPS are shown below.

The backend data storage leverages both HashMap and RocksDB: hashmap excels at handling increased random inquiries, while RocksDB, a mature and optimized business solution, is ideal for persistent data storage. Compared to using HashMap as the KV store, RocksDB delivers 2.5 times higher write QPS and an astonishing 25 times faster P99 latency.



## Getting Started

### Build (in root dir /raft_cpp) (modified CMakeLists.txt file)
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release 
    // if we want to run tests file uisng GDB, cmake .. -DCMAKE_BUILD_TYPE=Debug
    make -j8
    
### Running a cluster

### Lu: Noting must run goreman and redis inside of build dir

First install [goreman](https://github.com/mattn/goreman), which manages Procfile-based applications.
### Lu: may need set port in CLI
    export GRREMAN_RPC_PORT=8555
### then start with assigned port
    goreman start
    
Lu: using ctrl + C to exit goreman for the following installing (Redis)
    
### Test

install [redis-cli](https://github.com/antirez/redis), a redis console client.

[Lu:
### install redis and redis-cli
    sudo apt update (Ubuntu)
    sudo apt install reids-server

### running redis and check status
    sudo systemctl start redis
    sudo systemctl status redis

### check the port num: generally the port of Redis is 6379, not 63791. but we set the specific port number in Procfile
    sudo grep "port" /etc/redis/redis.conf
]

### run goreman in bash1. and open a new bash run this (remember to set same port in bash2)
    export GOREMAN_RPC_PORT=8555 // in both bashes. otherwise refuse connection via RPC

    redis-cli -p 63791  // enter command after 127.0.0.1:6379
    127.0.0.1:63791> set mykey myvalue
    OK
    127.0.0.1:63791> get mykey
    "myvalue"

### also for two bashes
remove a node and replace the myvalue with "new-value" to check cluster availability:
    Noting: keep bash1 goreman running all the time (don't ctrl + c to terminate the running goreman)
    goreman start  // in bash1. bash1 is for monitoring status change of redis cache

    goreman run stop node2  // in bash2
    // remove node2. you will see non-stoptable message indicating node1 and node3 cannot connet (error connection refused), indicating they lost connection with node2

    redis-cli -p 63791  // in bash2
    // we set value of node1 here. if node2 come back later, should see this update value

    127.0.0.1:63791> set mykey new-value
    OK
    
bring the node back up and verify it recovers with the updated value "new-value":
### bring node2 back first (in bash2)
    goreman run restart node2

    redis-cli -p 63792
    127.0.0.1:63792> KEYS *
    1) "mykey"
    127.0.0.1:63792> get mykey
    "new-value"

### test for rocksdb
To run all test after build, please run `ctest` in build dir. 
    
### benchmark
Please run `goreman start` in bash 1

Please run `redis-benchmark -t set,get -n 100000 -p 63791` in bash 2

**Using rocksdb as kv store** 

The stress test for RocksDB as the kv store shows 4500 QPS for 4KB KV write and 26000 QPS for 5KB kv read. The P99 delay is less than 40 ms. Compared with unordered_map as the kv store, QPS for KV write is **2.5 folds higher** and P99 is **25 folds faster**.
    
    ====== SET ======
        100000 requests completed in 22.34 seconds
        50 parallel clients
        3 bytes payload
        keep alive: 1
        multi-thread: no

    99.96% <= 43 milliseconds
    99.97% <= 44 milliseconds
    99.99% <= 45 milliseconds
    100.00% <= 52 milliseconds
    4476.08 requests per second

    ====== GET ======
        100000 requests completed in 3.84 seconds
        50 parallel clients
        3 bytes payload
        keep alive: 1
        multi-thread: no

    0.02% <= 1 milliseconds
    99.35% <= 2 milliseconds
    100.00% <= 2 milliseconds
    26041.67 requests per second

**using unordered map as kv store**

    ====== SET ======
        100000 requests completed in 55.53 seconds
        50 parallel clients
        3 bytes payload
        keep alive: 1
        multi-thread: no

    99.95% <= 1094 milliseconds
    99.98% <= 1097 milliseconds
    100.00% <= 1097 milliseconds
    1800.96 requests per second

    ====== GET ======
        100000 requests completed in 3.68 seconds
        50 parallel clients
        3 bytes payload
        keep alive: 1
        multi-thread: no

    99.99% <= 3 milliseconds
    100.00% <= 13 milliseconds
    27151.78 requests per second
    
    
    
    
    
    
    

