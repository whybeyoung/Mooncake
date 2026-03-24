# Offload Production Test Plan

## 背景与目标

本文档定义 Mooncake Real Client 模式下 `LOCAL_DISK` offload 的生产上线级测试方案。目标不是介绍功能，而是给出一份可执行、可验收、可复盘的上线验证规范，用于上线前联调、CI 回归、灰度放量和故障复现。

测试对象限定为：

- `mooncake_master --enable_offload=true`
- `mooncake_client --enable_offload=true`
- `MOONCAKE_OFFLOAD_*` 本地磁盘 offload 配置

不包含 master 共享 `DISK` replica 能力。

## 测试环境矩阵

以下组合至少覆盖一轮完整功能验证；上线阻断项至少在 bucket backend 上完成一轮通过。

| 维度 | 组合 |
|---|---|
| 网络 | 单机 TCP、双节点 TCP、双节点 RDMA |
| backend | `bucket_storage_backend`、`file_per_key_storage_backend`、`offset_allocator_storage_backend` |
| 容量模式 | 默认容量、显式总容量上限、bucket eviction |
| 生命周期 | 冷启动、client 重启、master 重启 |
| 压力模式 | 低并发验证、持续压测、soak test |

建议最小执行矩阵：

| 场景 | 网络 | backend | 目的 |
|---|---|---|---|
| S1 | 单机 TCP | bucket | 基线功能与容量验证 |
| S2 | 双节点 TCP | bucket | 远端 fallback load 与多客户端读取 |
| S3 | 双节点 RDMA | bucket | 生产链路性能与回读验证 |
| S4 | 单机 TCP | file_per_key | backend 兼容性与恢复 |
| S5 | 单机 TCP | offset_allocator | 高并发与“不恢复”预期验证 |

## 前置配置基线

### 基础进程

```bash
mooncake_master \
  --rpc_port=50051 \
  --enable_offload=true

export MOONCAKE_OFFLOAD_FILE_STORAGE_PATH=/nvme/mooncake_offload
export MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR=bucket_storage_backend

mooncake_client \
  --host=<node-ip> \
  --metadata_server=<metadata-entry> \
  --master_server_address=<master-entry> \
  --protocol=<tcp-or-rdma> \
  --device_names=<device-or-empty> \
  --port=50052 \
  --global_segment_size=4GB \
  --enable_offload=true
```

### 关键配置要求

- `MOONCAKE_OFFLOAD_FILE_STORAGE_PATH` 必须是存在、可写、绝对路径。
- `MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES` 在容量测试中必须显式设置，避免依赖默认 `2 TB`。
- bucket backend 容量淘汰测试必须显式设置：
  - `MOONCAKE_BUCKET_MAX_TOTAL_SIZE`
  - `MOONCAKE_BUCKET_EVICTION_POLICY`
- 压测与 soak test 必须开启日志落盘、metrics 抓取、磁盘使用量采集。

### 观测与产物

每轮测试至少采集：

- master/client 日志
- `/metrics` 或 `/metrics/summary`
- offload 目录 `du -sb`
- `HttpTestService` 返回的 replica 快照
- 执行脚本 stdout/stderr

## 执行模型

测试按“工具能力 + 外部编排”执行：

- `HttpTestService` 提供对象、副本、存在性、清理、benchmark 状态接口。
- 外部脚本负责起停进程、制造内存压力、注入故障、记录指标、做断言。

每个测试项统一按以下模板记录执行结果：

- 目标
- 环境
- 前置配置
- 步骤
- 观测点
- 通过标准
- 失败后保留物

## 功能测试

### F1. Offload 触发与 LOCAL_DISK 副本可见性

- 目标：验证内存压力达到阈值后，master 能触发 offload，对象出现 `LOCAL_DISK` replica。
- 环境：S1
- 前置配置：缩小 `--global_segment_size`，写入总量显著超过内存池。
- 步骤：
  1. 通过 HTTP 工具写入一批对象。
  2. 轮询 `GET /api/object/{key}/replicas`。
  3. 记录对象从仅 `MEMORY` 到出现 `LOCAL_DISK` 的转换。
- 观测点：
  - replica 列表中出现 `LOCAL_DISK`
  - `transport_endpoint` 非空
  - offload 目录文件数和占用增长
- 通过标准：
  - 目标对象在合理等待窗口内出现 `LOCAL_DISK`
  - 未出现错误状态 replica

### F2. SSD fallback load 正确性

- 目标：验证对象从 SSD 回读后数据内容和大小完全一致。
- 环境：S1、S2、S3
- 前置配置：对象已完成 offload。
- 步骤：
  1. 记录写入内容 checksum。
  2. 对目标对象执行 GET。
  3. 比较内容、size、checksum。
- 观测点：
  - HTTP GET 成功
  - 数据一致
  - 不出现未解释的 5xx
- 通过标准：
  - 内容完全一致
  - 连续多次读取结果一致

### F3. 多客户端读取

- 目标：验证双节点下其他 client 能读取 `LOCAL_DISK` 对象。
- 环境：S2、S3
- 前置配置：Node A 完成 offload，Node B 通过同一 master 访问。
- 步骤：
  1. 在 Node A 写入并触发 offload。
  2. 在 Node B 对同一 key 发起读取。
  3. 轮询副本和结果。
- 观测点：
  - Node B 读取成功
  - Node A 返回的 `transport_endpoint` 被使用
- 通过标准：
  - 跨节点回读成功且数据一致

### F4. 删除后清理

- 目标：验证对象删除后，元数据和查询结果收敛正确。
- 环境：S1
- 步骤：
  1. 写入并触发 offload。
  2. 删除对象。
  3. 轮询 `exists` 和 `replicas`。
- 观测点：
  - `exists=false`
  - 单 key replica 查询返回 `404`
- 通过标准：
  - 查询结果收敛
  - 后续读取返回 not found

## 容量与淘汰测试

### C1. 总容量上限

- 目标：验证 `MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES` 达到上限后的行为符合预期。
- 环境：S1、S4、S5
- 前置配置：显式设置较小上限。
- 步骤：
  1. 连续写入直到上限附近。
  2. 持续观察新的 offload 行为和查询结果。
- 观测点：
  - 新对象是否停止 offload 或出现受控失败
  - 目录大小是否超过上限
- 通过标准：
  - 不出现无限制增长
  - 行为与 backend 预期一致

### C2. Bucket eviction

- 目标：验证 bucket backend 在 `MOONCAKE_BUCKET_MAX_TOTAL_SIZE` 下按策略淘汰。
- 环境：S1、S2
- 前置配置：
  - `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR=bucket_storage_backend`
  - `MOONCAKE_BUCKET_MAX_TOTAL_SIZE=<small-limit>`
  - `MOONCAKE_BUCKET_EVICTION_POLICY=fifo|lru`
- 步骤：
  1. 写入多批对象，触发淘汰。
  2. 对热点/冷数据做读取。
  3. 查询 replica 与目录大小。
- 观测点：
  - `fifo` 下最旧 bucket 先淘汰
  - `lru` 下最近访问对象优先保留
- 通过标准：
  - 策略行为可复现
  - 不出现仍可查询但底层文件已删除的陈旧副本

### C3. keys 与 bucket 边界

- 目标：验证空对象、小对象、大对象、bucket size/key limit 边界。
- 环境：S1、S4、S5
- 通过标准：
  - 边界对象行为稳定
  - 无崩溃、无未解释的错误码

## 故障与恢复测试

### R1. Real client 重启恢复

- 目标：验证 bucket/file_per_key backend 在 client 重启后可重新扫描并恢复副本可见性。
- 环境：S1、S2、S4
- 步骤：
  1. 写入并完成 offload。
  2. 重启 real client。
  3. 轮询 replica 与读取结果。
- 观测点：
  - 重启后 `LOCAL_DISK` replica 重新可见
  - 已 offload 数据可读
- 通过标准：
  - 不需要人工修复即可恢复

### R2. Master 重启恢复

- 目标：验证 master 重启后副本状态重新收敛。
- 环境：S1、S2
- 通过标准：
  - client 心跳恢复后查询与读取正常

### R3. 路径异常

- 目标：验证 offload 路径不存在、无写权限、符号链接路径时行为受控。
- 环境：S1
- 通过标准：
  - client 启动失败或 offload 不可用时有清晰错误
  - 不出现 silent corruption

### R4. 磁盘空间不足

- 目标：验证磁盘打满时的写入、offload、读取行为。
- 环境：S1、S2
- 通过标准：
  - 无进程崩溃
  - 错误可观测
  - 已存在且未损坏的数据仍可按预期读取

### R5. Offset allocator 重启不恢复

- 目标：验证 offset allocator backend 的“不恢复”行为与文档一致。
- 环境：S5
- 通过标准：
  - 重启后旧 offload 对象不可依赖
  - 文档与实际一致

## 性能与稳定性测试

### P1. 冷读回源延迟

- 目标：评估对象从 `LOCAL_DISK` 回读时的 p50/p95/p99 延迟。
- 环境：S2、S3
- 观测点：
  - GET latency
  - 回读吞吐
  - 失败率

### P2. 持续压测

- 目标：在持续 put/get 压力下验证无数据错误、无异常失败增长。
- 环境：S1、S3
- 时长：至少 30 分钟
- 通过标准：
  - 无校验失败
  - 无显著 error spike

### P3. Soak test

- 目标：验证 24 小时内稳定性和资源收敛。
- 环境：S1 或 S3
- 观测点：
  - 错误率
  - RSS / FD / 目录大小
  - buffer GC 相关异常
- 通过标准：
  - 无持续增长的失败率
  - 无明显资源泄漏迹象

### P4. 高并发 buffer GC

- 目标：验证 `release_offload_buffer` 与 buffer GC 在线程并发下不出现数据错乱。
- 环境：S3、S5
- 通过标准：
  - 内容校验稳定
  - 无悬挂/重复释放迹象

## 上线准入标准

以下条件全部满足，offload 才允许进入灰度或生产：

- 功能正确性 100% 通过
- 无数据损坏、无错误副本状态、无未解释的 5xx
- 容量限制行为可预测，磁盘占用不失控
- 重启与故障恢复后状态收敛
- soak test 无持续增长的失败率、无明显资源泄漏
- 性能指标相对基线不出现阻断级退化

以下情况直接阻断上线：

- 读取返回错误数据
- replica 元数据与真实可读状态不一致
- 磁盘淘汰造成陈旧副本仍被对外服务
- 重启恢复不稳定或需要人工介入
- 长时间压测下错误率持续上升

## 执行产物要求

每轮测试必须保留以下产物，至少保留到问题关闭或上线完成：

- master/client 全量日志
- 关键时间点的 replica JSON 快照
- benchmark 结果
- 磁盘使用量记录
- metrics 导出结果
- 执行脚本和环境变量快照

## HTTP 测试工具使用建议

新增 HTTP 接口用于黑盒验证：

- `GET /api/service/config`
- `GET /api/object/{key}/replicas`
- `POST /api/objects/replicas`
- `POST /api/objects/exists`
- `POST /api/objects/delete_by_regex`
- `POST /api/objects/delete_all`

建议外部脚本的断言顺序：

1. `PUT /api/object/{key}` 写入对象
2. `GET /api/object/{key}/replicas` 轮询副本变化
3. `GET /api/object/{key}` 校验回读内容
4. `POST /api/objects/exists` 批量核对存在性与 size
5. `POST /api/objects/delete_by_regex` 或 `delete_all` 清理环境
