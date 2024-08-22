# raft-kv

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
please run `goreman start` in bash 1 
Please run `redis-benchmark -t set,get -n 100000 -p 63791` in bash 2

**Using rocksdb as kv store**, 99.95% is 20 folds faster than unordered_map as kv store. Throughput (set) requests 2.5 folds more than unordered_map kv store.  Completed 1000000 set request 2 folds faster han unordered_map kv store.
    
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
    
    
    
    
    
    
    

