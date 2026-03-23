# Mooncake Client 部署文档

本文档说明 Mooncake Client 的两种部署形态，以及各自的生命周期、参数、部署方式与适用场景。

本文覆盖两类 Client：

1. 集成在引擎进程中的 Client
2. 独立启动的 `mooncake_client` 进程

其中第二类额外覆盖：

- 作为 Extra Storage Capacity 组件时的部署方式
- 启动脚本
- 每个启动参数的默认值、含义和具体影响

## 1. Client 形态总览

Mooncake Store 的 Client 实际上有两套运行方式：

### 1.1 引擎内集成模式

这一类 Client 不单独起进程，而是直接集成在引擎或业务进程内部。

典型特点：

- 生命周期与引擎进程一致
- 引擎启动时创建，退出时一起销毁
- 适合 SDK 集成场景

这一类又可以细分为两种：

- 直接在引擎中初始化 `RealClient`
- 在引擎中初始化 `DummyClient`，并让它连接到同机独立的 `mooncake_client`

### 1.2 独立进程模式

这一类 Client 通过独立二进制 `mooncake_client` 启动。

典型特点：

- 独立进程生命周期，与引擎解耦
- 可作为一台机器上“额外存储容量”的提供者
- 可承载 offload / local disk / extra memory segment 等能力
- 同机引擎通常通过 `DummyClient` 与它配合

---

## 2. 模式一：集成在引擎里的 Client

这一节对应“Client 跟随引擎生命周期”的场景。

### 2.1 生命周期特征

无论是 `RealClient` 还是 `DummyClient`，只要是通过 SDK 直接集成到引擎里，它的生命周期都和引擎进程绑定：

- 引擎启动时初始化
- 引擎退出时自动清理
- 不需要额外管理独立进程

相关入口主要在：

- Python 封装：[store_py.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-integration/store/store_py.cpp)
- `RealClient` 定义：[real_client.h](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/include/real_client.h)
- `DummyClient` 定义：[dummy_client.h](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/include/dummy_client.h)

### 2.2 方式 A：引擎内直接集成 RealClient

这是最直接的 SDK 模式。Python 侧常用接口是：

```python
store.setup(
    local_hostname,
    metadata_server,
    global_segment_size,
    local_buffer_size,
    protocol,
    rdma_devices,
    master_server_addr,
    engine=None,
)
```

对应实现见 [store_py.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-integration/store/store_py.cpp)。

#### 参数说明

### `local_hostname`

- 类型：`str`
- 默认值：无，必填
- 含义：当前节点对外通告的主机名或 IP
- 具体影响：
  - Client 会用它向 Master 注册本机地址
  - 如果没有显式带端口，Mooncake 会自动选择一个可用端口
  - 这个地址必须能被数据面访问到

### `metadata_server`

- 类型：`str`
- 默认值：无，必填
- 含义：metadata server 地址
- 常见取值：
  - `http://<master-host>:8080/metadata`
  - `P2PHANDSHAKE`
- 具体影响：
  - 控制 Transfer Engine 如何发现/交换连接信息
  - 如果走 HTTP metadata，就要求 Master 开启 `enable_http_metadata_server`

### `global_segment_size`

- 类型：`int`
- 默认值：`16777216`（16 MiB）
- 含义：该节点向 Mooncake 注册的全局 segment 大小
- 具体影响：
  - 决定该 client 提供多少内存空间给 Mooncake Store
  - 值越大，可承载的远程 KV 容量越大

### `local_buffer_size`

- 类型：`int`
- 默认值：`16777216`（16 MiB）
- 含义：本地 buffer 大小
- 具体影响：
  - 用作本地数据暂存和零拷贝相关能力
  - 太小会影响大对象或高并发访问性能

### `protocol`

- 类型：`str`
- 默认值：`"tcp"`
- 可选值：
  - `tcp`
  - `rdma`
- 具体影响：
  - 决定 Client 到其它节点的数据传输协议
  - `rdma` 对网络与设备有更高要求，但性能更好

### `rdma_devices`

- 类型：`str`
- 默认值：`""`
- 含义：RDMA 设备名列表
- 具体影响：
  - 为空时允许 Mooncake 自动发现
  - 非空时会固定使用指定设备，例如 `mlx5_0,mlx5_1`

### `master_server_addr`

- 类型：`str`
- 默认值：`"127.0.0.1:50051"`
- 含义：Master RPC 地址
- 具体影响：
  - Client 的注册、查询、任务等控制面行为都依赖这个地址
  - 如果地址不可达，Client 初始化会失败

### `engine`

- 类型：可选对象
- 默认值：`None`
- 含义：可选的 TransferEngine 实例
- 具体影响：
  - 传入时可复用外部 engine
  - 不传时由 Client 自己创建内部数据面能力

#### 适用场景

- 引擎进程本身就希望直接承载 Mooncake segment
- 不希望额外维护独立 `mooncake_client` 进程
- 单机或简单多机集成场景

### 2.3 方式 B：引擎内集成 DummyClient

这种方式下，引擎进程不直接持有完整 RealClient，而是集成一个 `DummyClient`。  
`DummyClient` 本身不直接向 Master 注册存储能力，它依赖同机的独立 `mooncake_client` 进程。

Python 侧入口是：

```python
store.setup_dummy(
    mem_pool_size,
    local_buffer_size,
    server_address,
)
```

对应实现见 [store_py.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-integration/store/store_py.cpp)。

#### DummyClient 的角色

- 嵌在引擎进程里
- 通过本地 RPC 与 RealClient 通信
- 通过 IPC / shared memory 把本地 buffer 注册到 RealClient
- 本身不承担独立的集群注册职责

这也是你提到的“引擎里集成一个 DummyClient”的典型形态。

#### 参数说明

### `mem_pool_size`

- 类型：`int`
- 默认值：无，必填
- 含义：DummyClient 侧内存池大小
- 具体影响：
  - 影响本地对象操作的可用内存空间
  - 过小会限制引擎本地缓存/缓冲能力

### `local_buffer_size`

- 类型：`int`
- 默认值：无，必填
- 含义：DummyClient 本地共享内存 buffer 大小
- 具体影响：
  - 会通过 IPC 方式注册给 RealClient
  - 用于数据交换和本地 buffer 管理

### `server_address`

- 类型：`str`
- 默认值：无，必填
- 含义：同机独立 `mooncake_client` 的地址，通常是 `127.0.0.1:<port>`
- 具体影响：
  - DummyClient 启动时先连接这个地址
  - 如果独立 `mooncake_client` 未启动或地址不可达，DummyClient 初始化失败

#### 适用场景

- 引擎进程不希望直接承担完整 RealClient 职责
- 需要把“引擎生命周期”和“额外存储容量组件”解耦
- 需要启用同机独立 offload / extra storage 能力

---

## 3. 模式二：独立启动 `mooncake_client` 进程

这一节对应独立进程部署，目标是把 `mooncake_client` 作为一台机器上的存储能力组件运行。

### 3.1 角色定位

独立 `mooncake_client` 的职责主要是：

- 向 Master 注册本机可用 segment
- 对外提供本地 RPC 服务，供同机 DummyClient 调用
- 作为本机额外存储容量的提供者
- 在开启 offload 时，承载磁盘写入和回读逻辑

这也是“作为 Extra Storage Capacity 组件”的标准部署方式。

### 3.2 关键行为说明

`mooncake_client` 的 main 函数在 [real_client_main.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/real_client_main.cpp) 中。

它会做几件事：

1. 初始化 `RealClient`
2. 调用 `setup_internal(...)`
3. 启动 dummy client monitor
4. 在本机启动 RPC server，供同机 DummyClient 访问

需要特别注意一个代码层面的事实：

- `mooncake_client` 的本地 RPC 服务当前固定监听在 `127.0.0.1:<port>`
- 也就是说，它设计上就是给“同机引擎里的 DummyClient”访问的
- `--host` 不是 RPC server 的监听地址，而是该 client 向 Master / 集群通告的对外地址

这是部署时最容易误解的一点。

---

## 4. 独立 `mooncake_client` 的启动参数

除 [real_client_main.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/real_client_main.cpp) 中定义的参数外，还需要注意 [real_client.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/real_client.cpp) 里的 HTTP server 参数也会一起生效。

### `--host`

- 类型：`string`
- 默认值：`"0.0.0.0"`
- 含义：当前 Client 对外通告的主机地址
- 具体影响：
  - 用于向 Master 注册当前节点地址
  - 应填写其它节点真正可达的 IP 或主机名
  - 它不是本地 RPC 服务的监听地址

### `--metadata_server`

- 类型：`string`
- 默认值：`"http://127.0.0.1:8080/metadata"`
- 含义：metadata server 连接串
- 具体影响：
  - 决定 Transfer Engine 如何拿到元数据
  - 常见取值为 HTTP metadata 地址，或者 `P2PHANDSHAKE`

### `--device_names`

- 类型：`string`
- 默认值：`""`
- 含义：设备名列表
- 具体影响：
  - 在 RDMA 模式下可指定 NIC，例如 `mlx5_0`
  - 为空时依赖自动发现或默认逻辑

### `--master_server_address`

- 类型：`string`
- 默认值：`"127.0.0.1:50051"`
- 含义：Master RPC 地址
- 具体影响：
  - Client 所有控制面请求都会访问这个地址
  - 如果指向错误，会出现 `Connection refused`、`Client not available` 等错误

### `--protocol`

- 类型：`string`
- 默认值：`"tcp"`
- 可选值：
  - `tcp`
  - `rdma`
- 含义：数据面协议
- 具体影响：
  - 决定 Client 传输大对象时使用的网络协议
  - RDMA 模式需要正确配置设备和环境

### `--port`

- 类型：`int32`
- 默认值：`50052`
- 含义：RealClient 本地 RPC 服务端口
- 具体影响：
  - DummyClient 需要通过这个端口连接本地 `mooncake_client`
  - 该 RPC 服务实际绑定在 `127.0.0.1`
  - 主要供同机进程访问，不是对外跨机暴露的数据面地址

### `--global_segment_size`

- 类型：`string`
- 默认值：`"4 GB"`
- 含义：该节点向 Mooncake 提供的全局 segment 容量
- 具体影响：
  - 字符串会被解析为字节数
  - 值越大，能提供给 Mooncake 的额外内存容量越大
  - 对“Extra Storage Capacity”场景最关键

### `--threads`

- 类型：`int32`
- 默认值：`1`
- 含义：本地 Client RPC 服务线程数
- 具体影响：
  - 控制 `mooncake_client` 本地 RPC server 的并发处理能力
  - 主要影响同机 DummyClient 调用并发

### `--enable_offload`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用 offload 能力
- 具体影响：
  - 开启后，RealClient 会初始化 `FileStorage`
  - 需要结合 offload 环境变量一起使用
  - 适用于 SSD / 本地磁盘扩容场景

### `--enable_http_server`

- 类型：`bool`
- 默认值：`false`
- 含义：是否开启内嵌 HTTP server
- 具体影响：
  - 开启后，会暴露 `/health`、`/metrics`、`/metrics/summary`
  - 便于探活和监控

### `--http_port`

- 类型：`int32`
- 默认值：`9300`
- 含义：Client 内嵌 HTTP server 端口
- 具体影响：
  - 仅在 `enable_http_server=true` 时生效
  - 适合接入 K8s probe 或 Prometheus 采集

---

## 5. 独立 Client 相关环境变量

### 5.1 基础运行环境变量

这些变量不是 gflags，但和独立 Client 部署强相关：

### `MC_STORE_USE_HUGEPAGE`

- 默认值：未设置
- 作用：启用 hugepage 倾向
- 影响：
  - 设置后 RealClient 会尝试在适用协议下使用 hugepage

### `MC_STORE_CLIENT_SETUP_RETRIES`

- 默认值：`20`
- 作用：当自动选端口注册失败时的最大重试次数
- 影响：
  - host 未带端口时，RealClient 会自动选一个端口并重试注册
  - 值越大，对短暂冲突越宽容

### `MC_STORE_CLIENT_METRIC`

- 默认值：`1`
- 作用：控制 client metrics 是否开启
- 影响：
  - `0` 时彻底关闭 client metrics
  - 默认开启

### `MC_STORE_CLIENT_METRIC_INTERVAL`

- 默认值：`0`
- 作用：client metrics 定时输出间隔，单位秒
- 影响：
  - `0` 表示收集但不定期打印
  - 大于 0 时会周期性输出指标摘要

### 5.2 Offload 相关环境变量

当 `--enable_offload=true` 时，通常还需要关注这些变量：

- `MOONCAKE_OFFLOAD_FILE_STORAGE_PATH`
- `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR`
- `MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES`
- `MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES`
- `MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT`
- `MOONCAKE_OFFLOAD_HEARTBEAT_INTERVAL_SECONDS`
- `MOONCAKE_USE_URING`

详细说明可参考现有文档 [ssd-offload.md](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/docs/source/deployment/ssd-offload.md)。

---

## 6. 独立 Client 启动脚本

我另外补了一份启动脚本：

[scripts/run-mooncake-client.sh](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/scripts/run-mooncake-client.sh)

这个脚本使用环境变量组装 `mooncake_client` 的启动参数，适合容器和宿主机统一使用。

支持的主要环境变量如下：

- `MC_CLIENT_BIN`
- `MC_HOST`
- `MC_METADATA_SERVER`
- `MC_MASTER_SERVER_ADDRESS`
- `MC_PROTOCOL`
- `MC_DEVICE_NAMES`
- `MC_CLIENT_PORT`
- `MC_GLOBAL_SEGMENT_SIZE`
- `MC_CLIENT_THREADS`
- `MC_ENABLE_OFFLOAD`
- `MC_ENABLE_HTTP_SERVER`
- `MC_HTTP_PORT`

### 默认行为

- 默认 `MC_HOST=127.0.0.1`
- 默认 `MC_METADATA_SERVER=http://127.0.0.1:8080/metadata`
- 默认 `MC_MASTER_SERVER_ADDRESS=127.0.0.1:50051`
- 默认 `MC_PROTOCOL=tcp`
- 默认 `MC_CLIENT_PORT=50052`
- 默认 `MC_GLOBAL_SEGMENT_SIZE=4GB`
- 默认 `MC_CLIENT_THREADS=1`
- 默认 `MC_ENABLE_OFFLOAD=false`
- 默认 `MC_ENABLE_HTTP_SERVER=false`
- 默认 `MC_HTTP_PORT=9300`

---

## 7. 独立 Client 部署示例

### 7.1 最小 TCP 模式

```bash
export MC_CLIENT_BIN=./build/mooncake-store/src/mooncake_client
export MC_HOST=192.168.1.10
export MC_METADATA_SERVER=http://192.168.1.10:8080/metadata
export MC_MASTER_SERVER_ADDRESS=192.168.1.10:50051

./scripts/run-mooncake-client.sh
```

适用场景：

- 单机测试
- 引擎和 Master 在同一台机器
- 不使用 RDMA

### 7.2 RDMA 模式

```bash
export MC_CLIENT_BIN=./build/mooncake-store/src/mooncake_client
export MC_HOST=192.168.1.10
export MC_METADATA_SERVER=P2PHANDSHAKE
export MC_MASTER_SERVER_ADDRESS=192.168.1.10:50051
export MC_PROTOCOL=rdma
export MC_DEVICE_NAMES=mlx5_0
export MC_GLOBAL_SEGMENT_SIZE=64GB

./scripts/run-mooncake-client.sh
```

适用场景：

- 跨机高性能 KV 传输
- 机器具备 RDMA 网卡

### 7.3 作为 Extra Storage Capacity 组件部署

```bash
export MC_CLIENT_BIN=./build/mooncake-store/src/mooncake_client
export MC_HOST=192.168.1.20
export MC_METADATA_SERVER=P2PHANDSHAKE
export MC_MASTER_SERVER_ADDRESS=192.168.1.10:50051
export MC_PROTOCOL=rdma
export MC_DEVICE_NAMES=mlx5_0
export MC_CLIENT_PORT=50052
export MC_GLOBAL_SEGMENT_SIZE=128GB
export MC_ENABLE_HTTP_SERVER=true
export MC_HTTP_PORT=9300

./scripts/run-mooncake-client.sh
```

这个场景下：

- 该节点不一定运行主引擎
- 但它会向集群贡献 segment 容量
- 同机引擎如果需要访问它，可以通过 DummyClient 走本地 RPC

### 7.4 开启 Offload 的独立 Client

```bash
export MC_CLIENT_BIN=./build/mooncake-store/src/mooncake_client
export MC_HOST=192.168.1.20
export MC_METADATA_SERVER=P2PHANDSHAKE
export MC_MASTER_SERVER_ADDRESS=192.168.1.10:50051
export MC_PROTOCOL=rdma
export MC_DEVICE_NAMES=mlx5_0
export MC_GLOBAL_SEGMENT_SIZE=4GB
export MC_ENABLE_OFFLOAD=true

export MOONCAKE_OFFLOAD_FILE_STORAGE_PATH=/nvme/mooncake_offload
export MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR=bucket_storage_backend

./scripts/run-mooncake-client.sh
```

---

## 8. 引擎 + DummyClient + 独立 RealClient 的组合关系

如果采用“引擎内 DummyClient + 独立 `mooncake_client`”这套架构，关系如下：

1. Master 负责控制面和元数据
2. 独立 `mooncake_client` 负责向 Master 注册容量，并承载本机数据能力
3. 引擎进程内嵌 `DummyClient`
4. `DummyClient` 通过本地 RPC 和 IPC 与同机 `mooncake_client` 通信

它的优点是：

- 引擎和存储组件可以解耦
- 容量管理更集中
- 更容易把 offload / extra storage 能力独立出来

---

## 9. 部署检查清单

### 引擎内集成模式

- 确认 `master_server_addr` 可达
- 确认 `metadata_server` 与 Master 实际部署方式一致
- 如果使用 RDMA，确认 `protocol`、`rdma_devices` 和宿主机网络配置一致
- 如果使用 DummyClient，确认同机 `mooncake_client` 已启动

### 独立 `mooncake_client` 模式

- 确认 `--host` 填的是对外可达地址，而不是随意占位值
- 确认 `--master_server_address` 可达
- 确认 `--metadata_server` 与实际 metadata 方案一致
- 确认 `--global_segment_size` 与机器实际可用容量匹配
- 如果开启 HTTP server，确认 `http_port` 已放行
- 如果开启 offload，确认 `MOONCAKE_OFFLOAD_*` 配置完整
- 如果与 DummyClient 配合，确认同机访问 `127.0.0.1:<port>` 没问题

## 10. 总结

Mooncake Client 的部署本质上有两条路线：

- 如果你希望最简单地和引擎绑在一起，就在引擎内直接集成 Client
- 如果你希望把存储能力单独抽出来，或者让它承担额外容量/offload 角色，就独立部署 `mooncake_client`

你这次特别关心的两点，可以直接归纳为：

- 引擎内集成时，Client 生命周期和引擎一致
- 独立进程部署时，`mooncake_client` 更像是一个“本机存储代理 + 额外容量提供者”，而引擎内通常配一个 `DummyClient` 与之配合

