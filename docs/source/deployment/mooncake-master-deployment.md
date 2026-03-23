# Mooncake Master 部署文档

本文档基于 [master.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/master.cpp) 中实际定义的启动参数与启动流程编写，专门说明 `mooncake_master` 的部署方式与参数语义。

本文覆盖以下内容：

- `mooncake_master` 的启动流程
- 配置文件与命令行参数的优先级
- 所有启动参数
- 每个参数的默认值
- 每个参数对系统行为的具体影响
- 常见部署方式与部署检查项

## 1. Master 组件概述

`mooncake_master` 是 Mooncake Store 的控制面组件，主要负责：

- 管理对象元数据与 segment 元数据
- 对外提供 RPC 服务，接收 client 请求
- 暴露监控指标
- 按需内嵌 HTTP metadata server
- 在 HA 模式下通过 etcd 协调主视图与集群状态
- 按需执行 snapshot 持久化与恢复

## 2. 启动流程

`mooncake_master` 在 [master.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/master.cpp) 中的大致启动顺序如下：

1. 解析 gflags 命令行参数。
2. 如果设置了 `--config_path`，先加载 JSON/YAML 配置文件。
3. 将命令行参数覆盖到配置文件之上。
4. 进行基础校验：
   - 如果 `enable_ha=true`，则 `etcd_endpoints` 不能为空
   - `memory_allocator` 只能是 `cachelib` 或 `offset`
   - `eviction_ratio` 必须位于 `[0.0, 1.0]`
5. 如果启用了 `enable_http_metadata_server`，则先启动内嵌 HTTP metadata server。
6. 按运行模式启动服务：
   - HA 模式：启动 `MasterServiceSupervisor`
   - 非 HA 模式：直接启动 `coro_rpc_server`

## 3. 配置优先级

`mooncake_master` 同时支持配置文件和命令行参数。

- 如果没有设置 `--config_path`，则所有配置都来自命令行参数和程序内建默认值。
- 如果设置了 `--config_path`，则先加载配置文件，再应用命令行覆盖。
- 命令行只有在“显式传入”时，才会覆盖配置文件中的对应值。

需要特别注意两个兼容参数：

- `--port` 已废弃，建议使用 `--rpc_port`
- `--max_threads` 已废弃，建议使用 `--rpc_thread_num`

如果新旧参数同时设置：

- `rpc_port` 优先于 `port`
- `rpc_thread_num` 优先于 `max_threads`

## 4. 相关环境变量

这些变量不是 `master.cpp` 里的 gflags 参数，但它们会直接影响 Master 的运行行为。

### `MC_RPC_PROTOCOL`

- 默认值：未设置时视为 `tcp`
- 作用：控制 Master RPC 的底层传输协议
- 影响：
  - 默认走 TCP
  - 如果设置为 `rdma`，Master 启动时会调用 `server.init_ibv()` 初始化 RDMA 通道

### `MOONCAKE_SNAPSHOT_LOCAL_PATH`

- 默认值：无
- 作用：为 `snapshot_backend_type=local` 指定本地 snapshot 存储目录
- 影响：
  - 当 snapshot 后端为 `local` 时是必需项
  - 如果未设置，本地 snapshot backend 初始化会失败

### `MOONCAKE_AWS_*`

- 作用：为 `snapshot_backend_type=s3` 提供 S3 连接配置
- 影响：
  - 仅在 snapshot backend 为 `s3` 时生效
  - 具体变量定义见 [s3_helper.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/utils/s3_helper.cpp)

常见变量包括：

- `MOONCAKE_AWS_REGION`
- `MOONCAKE_AWS_S3_ENDPOINT`
- `MOONCAKE_AWS_BUCKET_NAME`
- `MOONCAKE_AWS_ACCESS_KEY_ID`
- `MOONCAKE_AWS_SECRET_ACCESS_KEY`

## 5. 启动参数总览

下面按功能分组，逐项说明 `master.cpp` 中定义的所有启动参数。

---

## 5.1 基础配置参数

### `--config_path`

- 类型：`string`
- 默认值：`""`
- 含义：Master 配置文件路径
- 具体影响：
  - 为空时，只使用命令行和内建默认值
  - 非空时，先加载配置文件，再应用命令行覆盖

### `--port`

- 类型：`int32`
- 默认值：`50051`
- 含义：废弃的 RPC 监听端口参数
- 具体影响：
  - 仅在 `--rpc_port` 未设置时作为兼容入口使用
  - 不建议继续使用

### `--max_threads`

- 类型：`int32`
- 默认值：`4`
- 含义：废弃的 RPC 工作线程数参数
- 具体影响：
  - 仅在 `--rpc_thread_num` 未设置时参与兼容逻辑
  - 实际生效值为 `min(max_threads, hardware_concurrency())`
  - 不建议继续使用

---

## 5.2 RPC 服务参数

### `--rpc_thread_num`

- 类型：`int32`
- 默认值：`0`
- 含义：推荐使用的 RPC 工作线程数
- 具体影响：
  - 大于 0 时，直接作为 RPC server 的线程数
  - 等于 0 时，回退到 `max_threads` 兼容逻辑
  - 线程数过小会限制并发能力
  - 线程数过大可能增加上下文切换与资源占用

### `--rpc_port`

- 类型：`int32`
- 默认值：`0`
- 含义：推荐使用的 RPC 监听端口
- 具体影响：
  - 大于 0 时，直接作为 Master 的 RPC 监听端口
  - 等于 0 时，回退到 `port`
  - 所有 client 都需要通过这个端口访问 Master

### `--rpc_address`

- 类型：`string`
- 默认值：`"0.0.0.0"`
- 含义：RPC 监听地址
- 具体影响：
  - `0.0.0.0` 表示绑定所有 IPv4 网卡
  - 如果只希望对某块网卡或某个容器网络开放服务，应显式设置具体地址
  - 在多机部署或 HA 部署中，该地址必须能被 client 实际访问到

### `--rpc_conn_timeout_seconds`

- 类型：`int32`
- 默认值：`0`
- 含义：RPC 空闲连接超时时间，单位秒
- 具体影响：
  - `0` 表示不超时
  - 正数表示空闲连接在超时后自动回收
  - 有助于限制大规模连接场景下的连接资源占用

### `--rpc_enable_tcp_no_delay`

- 类型：`bool`
- 默认值：`true`
- 含义：是否开启 `TCP_NODELAY`
- 具体影响：
  - 开启后，小包 RPC 延迟更低
  - 关闭后，可能减少发包次数，但会提高部分请求延迟

---

## 5.3 指标与监控参数

### `--enable_metric_reporting`

- 类型：`bool`
- 默认值：`true`
- 含义：是否启用 Master 周期性指标上报
- 具体影响：
  - 控制 Master 内部的周期性指标输出行为
  - 不等同于关闭 HTTP metrics 端点
  - 更偏向日志/内部统计层面的定期上报

### `--metrics_port`

- 类型：`int32`
- 默认值：`9003`
- 含义：HTTP metrics 服务端口
- 具体影响：
  - 决定 `/metrics` 和 `/metrics/summary` 的监听端口
  - 如果要接 Prometheus，这个端口必须可达

---

## 5.4 KV 生命周期与淘汰参数

### `--default_kv_lease_ttl`

- 类型：`uint64`
- 默认值：`5000` 毫秒
- 含义：KV 对象默认 lease TTL
- 具体影响：
  - 值越小，未完成对象会被更快视为过期
  - 值越大，对慢写入链路更宽容，但清理延迟也更大

### `--default_kv_soft_pin_ttl`

- 类型：`uint64`
- 默认值：`1800000` 毫秒，即 30 分钟
- 含义：KV 对象默认 soft pin TTL
- 具体影响：
  - soft pin 持续时间越长，对热点对象保留越有利
  - 同时也会降低系统的淘汰灵活性

### `--allow_evict_soft_pinned_objects`

- 类型：`bool`
- 默认值：`true`
- 含义：是否允许淘汰 soft pinned 对象
- 具体影响：
  - `true`：soft pinned 对象优先保留，但在必要时仍允许淘汰
  - `false`：淘汰策略更保守，可能在资源吃紧时提高分配失败概率

### `--eviction_ratio`

- 类型：`double`
- 默认值：`0.05`
- 含义：每次触发淘汰时淘汰对象的比例
- 具体影响：
  - 必须在 `[0.0, 1.0]` 范围内
  - 值小：每次淘汰较轻，但触发频率可能更高
  - 值大：一次回收更多空间，但系统抖动和对象 churn 也可能更明显

### `--eviction_high_watermark_ratio`

- 类型：`double`
- 默认值：`0.95`
- 含义：触发高水位淘汰的容量比例
- 具体影响：
  - 值越高，容量利用率越高，但余量越小
  - 值越低，会更早触发淘汰，降低打满容量后的阻塞风险

---

## 5.5 高可用与集群相关参数

### `--enable_ha`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用高可用模式
- 具体影响：
  - 开启后，Master 不再直接启动普通 RPC 服务，而是通过 `MasterServiceSupervisor` 启动
  - 开启后必须设置 `etcd_endpoints`
  - HA 模式下，etcd 负责主视图和协调信息

### `--etcd_endpoints`

- 类型：`string`
- 默认值：`""`
- 含义：etcd 地址列表，多个地址用分号分隔
- 具体影响：
  - `enable_ha=true` 时必填
  - 非 HA 模式下即使设置了，也不会被实际使用，只会打印 warning
  - 只用于 HA 协调，不用于 snapshot 持久化

### `--client_ttl`

- 类型：`int64`
- 默认值：`10` 秒
- 含义：client 最后一次 ping 后，被视为存活的时长
- 具体影响：
  - 主要在 HA 模式下有意义
  - 值越小，故障 client 识别越快
  - 值越大，对抖动和短暂网络异常更宽容

### `--cluster_id`

- 类型：`string`
- 默认值：`"mooncake_cluster"`
- 含义：逻辑集群标识
- 具体影响：
  - 用作集群级别的命名空间
  - 同一套集群建议保持稳定
  - 不同环境应使用不同 `cluster_id`，避免命名空间冲突

---

## 5.6 存储与分配策略参数

### `--root_fs_dir`

- 类型：`string`
- 默认值：`""`
- 含义：文件系统后端的根目录
- 具体影响：
  - 用于 DFS 或文件系统型 segment 场景
  - 为空表示未配置文件系统根目录
  - 如果依赖文件存储能力，应确保该路径有效且已挂载

### `--global_file_segment_size`

- 类型：`int64`
- 默认值：`9223372036854775807`
- 含义：全局文件 segment 容量上限
- 具体影响：
  - 默认近似等于不限制
  - 可用于限制挂载文件系统或 DFS 可被 Master 使用的容量

### `--memory_allocator`

- 类型：`string`
- 默认值：`"offset"`
- 含义：全局 segment 的内存分配器类型
- 可选值：
  - `offset`
  - `cachelib`
- 具体影响：
  - 非法值会导致 Master 启动失败
  - `offset` 是默认路径，也更贴合当前 snapshot 逻辑
  - `cachelib` 会切换到底层 cachelib 分配策略

### `--allocation_strategy`

- 类型：`string`
- 默认值：`"random"`
- 含义：segment 选择策略
- 可选值：
  - `random`
  - `free_ratio_first`
  - `cxl`
- 具体影响：
  - `random`：逻辑最简单，选择速度快
  - `free_ratio_first`：更偏向空闲比例高的 segment，通常更利于负载均衡
  - `cxl`：面向 CXL 场景的选择策略

### `--enable_offload`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用 offload 能力
- 具体影响：
  - Master 会参与 SSD/offload 相关协调逻辑
  - 应与 real client 侧的 offload 配置一起启用

### `--enable_disk_eviction`

- 类型：`bool`
- 默认值：`true`
- 含义：是否启用磁盘后端淘汰能力
- 具体影响：
  - 开启后，磁盘后端对象可以进入淘汰决策
  - 关闭后，磁盘侧保留更保守，可能提高写入失败概率

### `--quota_bytes`

- 类型：`uint64`
- 默认值：`0`
- 含义：存储后端配额
- 具体影响：
  - `0` 表示使用默认配额，即容量的 90%
  - 大于 0 时表示显式限制后端可用容量
  - 适合用于保护磁盘或挂载卷不被打满

---

## 5.7 HTTP Metadata Server 参数

### `--enable_http_metadata_server`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用内嵌 HTTP metadata server
- 具体影响：
  - 开启后，Master 会在 RPC 服务前启动一个内嵌 HTTP metadata server
  - 可替代单独部署 metadata service 的方式

### `--http_metadata_server_port`

- 类型：`int32`
- 默认值：`8080`
- 含义：HTTP metadata server 监听端口
- 具体影响：
  - 仅在 `enable_http_metadata_server=true` 时生效
  - client 如果使用 HTTP metadata，应访问 `http://<host>:<port>/metadata`

### `--http_metadata_server_host`

- 类型：`string`
- 默认值：`"0.0.0.0"`
- 含义：HTTP metadata server 监听地址
- 具体影响：
  - 仅在 `enable_http_metadata_server=true` 时生效
  - 控制 metadata server 对外暴露在哪些网卡上

---

## 5.8 PutStart 与空间回收参数

### `--put_start_discard_timeout_sec`

- 类型：`uint64`
- 默认值：`30` 秒
- 含义：未完成 `PutStart` 请求的丢弃超时时间
- 具体影响：
  - 值越小，异常中断的写入会被更快回收
  - 值越大，对慢写入更宽容，但临时状态会保留更久

### `--put_start_release_timeout_sec`

- 类型：`uint64`
- 默认值：`600` 秒
- 含义：未完成 `PutStart` 已预留空间的释放超时时间
- 具体影响：
  - 控制“已分配但未提交”的空间能保留多久
  - 值越小，悬挂容量回收更快
  - 值越大，对慢数据通路更安全

---

## 5.9 Snapshot 参数

### `--snapshot_backup_dir`

- 类型：`string`
- 默认值：`""`
- 含义：snapshot/restore 的本地备份目录
- 具体影响：
  - 为空时，不启用本地备份
  - 非空时，可作为 snapshot 失败后的本地兜底或 restore 本地缓存目录

### `--enable_snapshot_restore`

- 类型：`bool`
- 默认值：`false`
- 含义：启动时是否从最新 snapshot 恢复
- 具体影响：
  - 开启后，Master 启动前会尝试恢复已有 snapshot
  - 如果 snapshot backend 配置不合法，启动会失败

### `--enable_snapshot`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用周期性 snapshot
- 具体影响：
  - 开启后，Master 会定时持久化元数据
  - 适合有重启恢复需求的部署场景

### `--snapshot_interval_seconds`

- 类型：`uint64`
- 默认值：`600` 秒
- 含义：snapshot 周期
- 具体影响：
  - 越小：恢复点目标越好，但 snapshot 开销更高
  - 越大：运行开销更低，但宕机时丢失窗口更大

### `--snapshot_child_timeout_seconds`

- 类型：`uint64`
- 默认值：`300` 秒
- 含义：单次 snapshot 子进程超时时间
- 具体影响：
  - 防止 snapshot 过程无限挂住
  - 过小会误杀大 snapshot
  - 过大会延迟错误发现

### `--snapshot_retention_count`

- 类型：`uint32`
- 默认值：`2`
- 含义：保留最近多少份 snapshot
- 具体影响：
  - 超过数量的老 snapshot 会被自动清理
  - 保留更多快照意味着更高存储占用，但也有更强回滚能力

### `--snapshot_backend_type`

- 类型：`string`
- 默认值：`""`
- 含义：snapshot 存储后端类型
- 可选值：
  - `local`
  - `s3`
- 具体影响：
  - 只影响 snapshot 持久化与恢复
  - 不影响 HA 元数据存储
  - `local` 需要设置 `MOONCAKE_SNAPSHOT_LOCAL_PATH`
  - `s3` 需要编译时启用 AWS SDK，并配置 `MOONCAKE_AWS_*`

---

## 5.10 Task Manager 参数

### `--max_total_finished_tasks`

- 类型：`uint32`
- 默认值：`10000`
- 含义：内存中最多保留多少个已完成任务
- 具体影响：
  - 值越大，能保留更多历史任务信息
  - 值越小，内存占用更低，但已完成任务会更早被清理

### `--max_total_pending_tasks`

- 类型：`uint32`
- 默认值：`10000`
- 含义：内存中最多允许多少个 pending task
- 具体影响：
  - 达到上限后，新任务可能被拒绝
  - 突发任务量高时可以适当调大

### `--max_total_processing_tasks`

- 类型：`uint32`
- 默认值：`10000`
- 含义：内存中最多允许多少个 processing task
- 具体影响：
  - 限制系统内部同时处理中任务的规模
  - 防止任务并发过高导致资源失控

### `--pending_task_timeout_sec`

- 类型：`uint64`
- 默认值：`300` 秒
- 含义：pending task 超时时间
- 具体影响：
  - `0` 表示不超时
  - 可用于自动清理长时间未被处理的任务

### `--processing_task_timeout_sec`

- 类型：`uint64`
- 默认值：`300` 秒
- 含义：processing task 超时时间
- 具体影响：
  - `0` 表示不超时
  - 用于防止任务长期卡死在处理中状态

### `--max_retry_attempts`

- 类型：`uint32`
- 默认值：`10`
- 含义：失败任务最大重试次数
- 具体影响：
  - 值越大，对临时性故障容忍度越高
  - 但最终失败的确认时间也会更长

---

## 5.11 CXL 参数

### `--cxl_path`

- 类型：`string`
- 默认值：`"/dev/dax0.0"`
- 含义：CXL DAX 设备路径
- 具体影响：
  - 仅在启用 CXL 时有意义
  - 必须指向宿主机上真实存在的 DAX 设备

### `--cxl_size`

- 类型：`uint64`
- 默认值：`8589934592` 字节，即 8 GiB
- 含义：CXL 可用容量
- 具体影响：
  - 仅在启用 CXL 时有意义
  - 决定 Master 管理的 CXL 空间大小

### `--enable_cxl`

- 类型：`bool`
- 默认值：`false`
- 含义：是否启用 CXL 能力
- 具体影响：
  - 开启后会启用 CXL 相关分配与设备逻辑
  - 仅适用于具备相应硬件与设备节点的环境

## 6. 重要运行说明

### 6.1 RPC 协议选择不是命令行参数

Master 的 RPC 协议不是通过 gflags 指定，而是通过环境变量控制：

- 默认：`tcp`
- 设置 `MC_RPC_PROTOCOL=rdma` 后，Master 会初始化 RDMA RPC

示例：

```bash
export MC_RPC_PROTOCOL=rdma
./build/mooncake-store/src/mooncake_master --rpc_port=50051 --rpc_thread_num=64
```

### 6.2 Snapshot 与 etcd 不是一回事

需要明确区分：

- `enable_ha=true` 时，HA 元数据和主视图协调走 etcd
- snapshot 只会走 `local` 或 `s3`

也就是说：

- snapshot 不走 etcd
- etcd 也不是 snapshot backend

### 6.3 日志初始化行为

在 [master.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/mooncake-store/src/master.cpp) 中，只有在 `FLAGS_log_dir` 非空时才会调用 `google::InitGoogleLogging(argv[0])`。因此如果没有配置 glog 输出目录，部分早期日志会直接打到 stderr。

## 7. 部署示例

### 7.1 单机最小部署

适用于本地测试或单机场景：

```bash
./build/mooncake-store/src/mooncake_master \
  --rpc_port=50051 \
  --rpc_thread_num=8 \
  --enable_http_metadata_server=true \
  --http_metadata_server_host=0.0.0.0 \
  --http_metadata_server_port=8080 \
  --metrics_port=9003
```

效果：

- 启动一个非 HA Master
- RPC 对外监听 `50051`
- 内嵌 metadata server 对外监听 `8080`
- metrics 对外监听 `9003`

### 7.2 生产环境常规部署

适用于对外暴露 HTTP metadata 且需要监控的场景：

```bash
./build/mooncake-store/src/mooncake_master \
  --rpc_address=0.0.0.0 \
  --rpc_port=50051 \
  --rpc_thread_num=64 \
  --rpc_conn_timeout_seconds=30 \
  --rpc_enable_tcp_no_delay=true \
  --enable_metric_reporting=true \
  --metrics_port=9003 \
  --enable_http_metadata_server=true \
  --http_metadata_server_host=0.0.0.0 \
  --http_metadata_server_port=8080 \
  --eviction_ratio=0.05 \
  --eviction_high_watermark_ratio=0.95
```

### 7.3 HA 部署

适用于需要高可用与主视图协调的场景：

```bash
./build/mooncake-store/src/mooncake_master \
  --enable_ha=true \
  --etcd_endpoints="10.0.0.11:2379;10.0.0.12:2379;10.0.0.13:2379" \
  --rpc_address=10.0.0.21 \
  --rpc_port=50051 \
  --rpc_thread_num=64 \
  --cluster_id=prod_cluster_a \
  --metrics_port=9003
```

效果：

- Master 通过 etcd 维护 HA 主视图
- client 的存活由 `client_ttl` 决定
- `cluster_id` 用于隔离不同集群命名空间

### 7.4 开启本地 Snapshot 的部署

```bash
export MOONCAKE_SNAPSHOT_LOCAL_PATH=/data/mooncake_snapshots

./build/mooncake-store/src/mooncake_master \
  --rpc_port=50051 \
  --rpc_thread_num=32 \
  --enable_snapshot=true \
  --snapshot_backend_type=local \
  --snapshot_interval_seconds=600 \
  --snapshot_retention_count=3 \
  --snapshot_backup_dir=/data/mooncake_snapshot_backup
```

效果：

- Master 周期性持久化元数据
- 保留最近 3 份 snapshot
- 同时保留一份本地备份目录作为兜底

## 8. 部署检查清单

正式部署前建议至少确认以下事项：

- `rpc_port` 对所有 client 可达
- 如果启用了 HTTP metadata，`http_metadata_server_port` 对 client 可达
- 如果启用了 HA，所有 `etcd_endpoints` 稳定可达
- 如果使用 RDMA，已经设置 `MC_RPC_PROTOCOL=rdma` 且 RDMA 设备可正常使用
- 如果使用本地 snapshot，已设置 `MOONCAKE_SNAPSHOT_LOCAL_PATH`
- 如果使用 S3 snapshot，已确认编译时包含 AWS SDK，且 `MOONCAKE_AWS_*` 配置完整
- 尽量显式设置 `rpc_thread_num`，不要依赖废弃参数 `max_threads`
- 尽量使用 `rpc_port`，不要继续使用废弃参数 `port`

## 9. 总结

`mooncake_master` 的参数总体可以分为几类：

- RPC 与网络监听参数
- 指标与监控参数
- KV 生命周期与淘汰参数
- HA 与集群参数
- 存储与分配策略参数
- HTTP metadata 参数
- snapshot 参数
- task manager 参数
- CXL 参数

部署时建议先明确你的目标场景：

- 单机测试
- 常规生产部署
- HA 部署
- snapshot 持久化部署
- CXL / offload 特殊部署

然后只启用对应参数，避免把彼此无关的能力同时打开，增加排障复杂度。
