# mooncake_dummy_test_service

`mooncake_dummy_test_service` 是一个独立的 Mooncake HTTP 测试工具。

它通过 `DummyClient` 连接已经部署好的 standalone `realclient`，并对外提供一组简单的 HTTP 接口，用于：

- 健康检查
- 对象写入、读取、删除
- 对象元信息查询
- benchmark 启动、停止、状态查询

这个工具被刻意放在 `tools/mooncake_dummy_test_service` 下单独维护，使用时不需要修改主工程的业务代码路径。

## 工具用途

当你已经具备以下运行环境时，这个工具会很方便：

- Mooncake master 已启动
- standalone realclient / storage 服务已启动

此时你可以用它来：

- 验证端到端读写链路是否正常
- 用 `curl` 做简单集成测试
- 对已部署服务发起对象读写 benchmark

## 构建方式

推荐使用独立构建目录：

```bash
mkdir -p build-dummy
cd build-dummy
cmake ../tools/mooncake_dummy_test_service
make -j$(nproc)
```

生成的可执行文件为：

```bash
./mooncake_dummy_test_service
```

如果不确定二进制产物在哪，可以搜索：

```bash
find . -name mooncake_dummy_test_service -type f
```

## 运行前准备

启动该工具前，请确认目标机器上已经具备：

- 可访问的 Mooncake master
- 可访问的 standalone `realclient`
- `realclient` 已监听你准备传入的 RPC 地址

默认的 `realclient` 地址是：

```text
127.0.0.1:50052
```

## 启动方式

最小启动示例：

```bash
./mooncake_dummy_test_service \
  --http_host=0.0.0.0 \
  --http_port=18080 \
  --real_client_address=127.0.0.1:50052
```

## 启动参数

参数定义见 [main.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/main.cpp)。

- `--http_host`
  HTTP 监听地址，默认值：`0.0.0.0`
- `--http_port`
  HTTP 监听端口，默认值：`18080`
- `--real_client_address`
  standalone `realclient` 的 RPC 地址，默认值：`127.0.0.1:50052`
- `--ipc_socket_path`
  `DummyClient` 使用的 IPC socket 路径，默认值为空
- `--mem_pool_size`
  Dummy client 内存池大小，单位字节，默认值：`268435456`
- `--local_buffer_size`
  Dummy client 本地 buffer 大小，单位字节，默认值：`268435456`
- `--default_replica_num`
  直接 `PUT` 时使用的默认副本数，默认值：`1`

当 `--ipc_socket_path` 为空时，服务会根据 `real_client_address` 自动推导：

```text
@mooncake_client_<port>.sock
```

例如：

```text
127.0.0.1:50052 -> @mooncake_client_50052.sock
```

## HTTP 接口说明

### 1. 健康检查

请求：

```bash
curl http://127.0.0.1:18080/health
```

示例响应：

```json
{
  "ipc_socket_path": "@mooncake_client_50052.sock",
  "ready": true,
  "real_client_address": "127.0.0.1:50052",
  "status": "healthy"
}
```

### 2. 写入对象

请求：

```bash
curl -X PUT http://127.0.0.1:18080/api/object/test1 -d 'hello'
```

可选地指定副本数：

```bash
curl -X PUT 'http://127.0.0.1:18080/api/object/test1?replica_num=2' -d 'hello'
```

示例响应：

```json
{
  "code": 0,
  "key": "test1",
  "size": 5
}
```

### 3. 读取对象

请求：

```bash
curl http://127.0.0.1:18080/api/object/test1
```

示例响应体：

```text
hello
```

### 4. 查询对象元信息

请求：

```bash
curl http://127.0.0.1:18080/api/object/test1/meta
```

示例响应：

```json
{
  "key": "test1",
  "exists": true,
  "size": 5
}
```

### 5. 删除对象

请求：

```bash
curl -X DELETE http://127.0.0.1:18080/api/object/test1
```

成功响应示例：

```json
{
  "code": 0,
  "key": "test1"
}
```

### 6. 启动 Benchmark

请求：

```bash
curl -X POST http://127.0.0.1:18080/api/benchmark/start \
  -H 'Content-Type: application/json' \
  -d '{
    "key_prefix": "bench",
    "object_size": 4096,
    "batch_size": 16,
    "iterations": 100,
    "concurrency": 4,
    "cleanup": true,
    "replica_num": 1,
    "seed": 12345
  }'
```

必填字段：

- `object_size`
- `batch_size`
- `iterations`
- `concurrency`
- `replica_num`

这些字段都必须大于 `0`。

可选字段：

- `key_prefix`
- `cleanup`
- `seed`

响应内容为当前 benchmark 快照，包含运行状态和统计信息。

### 7. 停止 Benchmark

请求：

```bash
curl -X POST http://127.0.0.1:18080/api/benchmark/stop
```

### 8. 查询 Benchmark 状态

请求：

```bash
curl http://127.0.0.1:18080/api/benchmark/status
```

响应中常见字段包括：

- `state`
- `run_id`
- `config`
- `elapsed_ms`
- `total_put_ops`
- `total_get_ops`
- `bytes_written`
- `bytes_read`
- `put_failures`
- `get_failures`
- `verify_failures`
- `cleanup_failures`
- `put_ops_per_sec`
- `get_ops_per_sec`
- `write_mib_per_sec`
- `read_mib_per_sec`
- `avg_batch_put_ms`
- `avg_batch_get_ms`
- `p50_batch_put_ms`
- `p95_batch_put_ms`
- `p50_batch_get_ms`
- `p95_batch_get_ms`
- `last_error`

## 推荐烟测流程

```bash
curl http://127.0.0.1:18080/health
curl -X PUT http://127.0.0.1:18080/api/object/test1 -d 'hello'
curl http://127.0.0.1:18080/api/object/test1
curl http://127.0.0.1:18080/api/object/test1/meta
sleep 2
curl -X DELETE http://127.0.0.1:18080/api/object/test1
```

## 重要说明

### 对象刚写完时，删除可能失败

底层后端可能会在对象刚写入后拒绝删除，并返回：

```text
-706
```

这个错误码对应 `OBJECT_HAS_LEASE`，表示对象当前仍持有 lease。

实际表现通常是：

- `PUT` 成功
- 紧接着执行 `DELETE` 可能失败
- 等待一小段时间后重试，通常可以成功

### Benchmark 运行期间会阻止普通对象接口

当 benchmark 正在运行时，服务会对直接对象接口返回 `409 Conflict`，避免 benchmark 过程和人工读写互相干扰。

### 对象 key 当前按单个路径段匹配

当前对象路由形式为：

```text
/api/object/<key>
/api/object/<key>/meta
```

目前的路由匹配逻辑要求 `<key>` 保持在单个 URL path segment 内。

如果 key 中包含特殊字符，请在请求时先进行 URL 编码。

## 常见排查

### `/health` 返回 not ready

请优先检查：

- `realclient` 是否已经启动
- `--real_client_address` 是否正确
- 自动推导或手工指定的 IPC socket path 是否正确

### PUT/GET 成功，但日志提示 hot cache unavailable

这通常不是致命问题。只要普通数据通路是通的，这个工具仍然可以正常工作。

### `/api/object/<key>/meta` 返回了异常的对象查找结果

请确认当前运行的是最新重新编译后的二进制。旧版本里更宽泛的路由匹配可能会错误处理 `/meta` 请求。

## 代码位置

- [CMakeLists.txt](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/CMakeLists.txt)
- [main.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/main.cpp)
- [http_test_service.h](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/http_test_service.h)
- [http_test_service.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/http_test_service.cpp)
- [benchmark_runner.h](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/benchmark_runner.h)
- [benchmark_runner.cpp](/Users/jicheng/Documents/ASE/kvcache-ai/Mooncake/tools/mooncake_dummy_test_service/src/benchmark_runner.cpp)
