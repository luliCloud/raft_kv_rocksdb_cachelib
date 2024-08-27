# Raft-KV Product

This product provides robust key-value storage tailored for distributed database systems, engineered to meet the demands of applications requiring **high availability**, **data consistency**, and exceptional **fault tolerance**, even in network partition scenarios. It’s an ideal solution for industries like **finance**, **e-commerce**, and **social media**.

Our stress tests demonstrated impressive performance, with 4500 QPS for 4KB KV writes and 26000 QPS for 5KB KV reads, all while maintaining a P99 latency of less than 40 milliseconds. The performance across different size of QPS are shown below.

The backend data storage leverages both HashMap and RocksDB: hashmap excels at handling increased random inquiries, while RocksDB, a mature and optimized business solution, is ideal for persistent data storage. Compared to using HashMap as the KV store, RocksDB delivers 2.5 times higher write QPS and an astonishing 25 times faster P99 latency.



## Getting Started

### Build
To build the project, follow these steps in the root directory `/raft_cpp`:

    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release 
    make -j8
    
Note: If you want to run test files using GDB, use the following command:

    cmake .. -DCMAKE_BUILD_TYPE=Debug


### Running a cluster
**Important**: Ensure that `goreman` and `redis` are run inside the `build` directory.

1. **Install Goreman**\
First install [goreman](https://github.com/mattn/goreman), which manages Procfile-based applications.
2. **Set RPC Port (Mandatory for IPC)**\
If you need to set a specific port in the CLI, run in both bashes:

    ```
    export GRREMAN_RPC_PORT=8555
3. **Start Goreman**\
start the cluster with the assigned port:
    ```
    goreman start
4. **Exit Goreman**\
Use `ctrl + c` to exit `goreman` before proceeding with Redis installation.
    
### Install Redis
1. **Install Redis and Redis-CLI**\
install [redis-cli](https://github.com/antirez/redis), a redis console client. To install Redis and the Redis CLI:
    ```
    sudo apt update #(for Ubuntu)
    sudo apt install reids-server
2. **Start Redis and check status**
    ```
    sudo systemctl start redis
    sudo systemctl status redis

### Running Tests
**Run Goreman in Bash**\
1. Open a terminal (bash1) and run:
    ``` 
    export GOREMAN_RPC_PORT=8555
    goreman start
2. Open a new terminal (bash2) and run the following (remember to set the same port in both bash sessions):
    ```
    export GOREMAN_RPC_PORT=8555
    redis-cli -p 63791
3. Example commands to test:
    ```
    127.0.0.1:63791> set mykey myvalue
    OK
    127.0.0.1:63791> get mykey
    "myvalue"

**Test Cluster Availability**
1. In bash1, keep goreman running (do not terminate it with ctrl + C).
2. In bash2, stop node2:
    ```
    goreman run stop node2
3. You should see error messages indicating that node1 and node3 cannot connect with node2 (Msg: `connect refused`), as they lost connection with node2.
4. Set a new value in node1 when node2 exits the cluster (mimic node failure and network partitions):
    ```
    redis-cli -p 63791
    127.0.0.1:63791> set mykey new-value
    OK
5. Bring node2 back up and verify it recovers with the updated value:
    ```
    goreman run restart node2
    redis-cli -p 63792
    127.0.0.1:63792> KEYS *
    1) "mykey"
    127.0.0.1:63792> get mykey
    "new-value"

### Test for rocksdb
To run all tests after building the project, navigate to the build directory and execute: 
    ```
    ctest```
To run specific test files, navigate to the `/build/tests` directories and run `./testfile` (if the testfile named as this).

### Benchmarking
1. **Start Goreman In bash1**:
    ```
    goreman start
2. **Run Redis Benchmark in bash2**:
    ```
    redis-benchmark -t set,get -n 100000 -p 63791
### Performance with RocksDB as the KV Store
The stress test for RocksDB as the key-value store shows:
1. 4500 QPS for 4KB KV writes.
2. 26000 QPS for 5KB KV reads.
3. P99 latency is less than 40 ms.

Compared to using unordered_map as the KV store, RocksDB provides:
1. 2.5 times higher QPS for KV writes.
2. 25 times faster P99 latency.\
**Using RocksDB as kv store**

**Use unordered map as kv store**

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

**Use unordered map as kv store**

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
    
    
    
    
    
    
    

