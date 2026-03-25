# Mooncake Connector

Mooncake Connector 是一个动态库工具，用于以特定接口形式协作 dummy-client 和 real-client，提供给接口已经固定的服务使用。

## 目录结构

```
tools/connector/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 本文档
├── include/                # 公共头文件
│   ├── aiges_type.h        # 基础类型定义
│   ├── kvconnector.h       # KVConnector 类声明
│   └── kvconnector_wrapper.h  # C 接口封装
└── src/                    # 实现文件
    ├── kvconnector.cpp     # KVConnector 类实现
    └── kvconnector_wrapper.cpp  # C 接口实现
```

## 构建方式

connector 作为 Mooncake 项目的子项目编译，不支持单独构建。

### 从项目根目录构建

```bash
cd /path/to/Mooncake
cmake -S . -B build \
    -DWITH_STORE=ON \
    -DWITH_TE=ON \
    -DBUILD_CONNECTOR=ON
cmake --build build
```

### 依赖选项

- `WITH_STORE=ON`: 启用 Mooncake Store 支持（必需）
- `WITH_TE=ON`: 启用 Transfer Engine 支持（必需）
- `BUILD_CONNECTOR=ON`: 构建 connector 库

## 输出产物

- **动态库**: `libmooncake_connector_sdk.so`
- **头文件**: `include/mooncake_connector/` 目录下

## API 参考

### C++ 接口

```cpp
#include "kvconnector.h"

// 创建连接器
KVConnector connector;

// 初始化
int rc = connector.setup(
    "localhost",          // local_hostname
    "etcd://localhost:2379",  // metadata_server
    "tcp",                // protocol
    "eth0",               // rdma_devices
    "localhost:50051",    // master_server_addr
    1073741824,           // global_segment_size (1GB)
    268435456,            // local_buffer_size (256MB)
    "cachelib"            // memory_allocator
);

// Put 操作
rc = connector.put("key", "value", 1, "");

// Get 操作
bytes value = connector.get("key");

// Remove 操作
rc = connector.remove("key");
```

### C 接口

```c
#include "kvconnector_wrapper.h"

// 配置结构
MoonCakeClientConfig config = {
    .local_hostname = "localhost",
    .metadata_server = "etcd://localhost:2379",
    .protocol = "tcp",
    .master_server_addr = "localhost:50051",
    .device_name = "eth0",
    .global_segment_size = 1073741824,
    .local_buffer_size = 268435456,
    .replica_num = 1,
    .log_level = "INFO",
    .log_dir = "./logs",
    .max_log_size = 100,
    .memory_allocator = "cachelib"
};

// 初始化处理器
CacheHandler handler;
int rc = init_mooncake_handler(&handler, &config);

// 使用 handler 进行缓存操作
// handler.put(...), handler.get(...), etc.

// 销毁处理器
destroy_mooncake_handler();
```

## 错误码

| 错误码 | 值 | 描述 |
|--------|-----|------|
| KVCONNECTOR_SUCCESS | 0 | 操作成功 |
| KVCONNECTOR_ERR_BACKEND_OP | -1 | 后端操作失败 |
| KVCONNECTOR_ERR_DATA_NOT_FOUND | -2 | 数据未找到 |
| KVCONNECTOR_ERR_KEY_INVALID | -3 | 键无效 |
| KVCONNECTOR_ERR_PARTIAL_EXIST | -4 | 部分存在 |
| KVCONNECTOR_ERR_NOT_EXIST | -5 | 全部不存在 |
| KVCONNECTOR_ERR_PARTIAL_DEL | -6 | 部分删除成功 |
| KVCONNECTOR_ERR_INVALID_PARAM | -7 | 参数无效 |
| KVCONNECTOR_ERR_MEMORY_ALLOC | -8 | 内存分配失败 |
| KVCONNECTOR_ERR_CONNECTOR_DESTROYED | -9 | 连接器已销毁 |

## 日志配置

使用 glog 进行日志记录，支持以下配置：

- 日志级别：INFO, WARNING, ERROR
- 日志目录：通过 `log_dir` 参数配置
- 单文件大小：通过 `max_log_size` 参数配置（MB）

## 注意事项

1. **线程安全**: `init_mooncake_handler` 和 `destroy_mooncake_handler` 是线程安全的
2. **资源管理**: 使用完毕后必须调用 `destroy_mooncake_handler()` 释放资源
3. **内存分配**: 确保 `local_buffer_size` 足够大以容纳预期的数据

## 变更历史

- 删除了未使用的 `thread_pool` 模块
- 优化了 CMake 配置，作为子项目编译
- 添加了日志记录功能
