# Mooncake Offload 机制详解

本文档详细解读 Mooncake 的 Offload（数据卸载）机制，包括架构设计、核心组件、配置参数和数据流程。

## 1. 概述

Offload 机制是 Mooncake 的核心特性之一，用于将内存中的 KV 缓存数据持久化到本地磁盘，实现以下目标：

- **容量扩展**：突破内存容量限制，支持更大规模的 KV 缓存
- **成本优化**：利用低成本磁盘存储热数据溢出部分
- **数据持久化**：服务重启后可快速恢复缓存状态

## 2. 架构设计

### 2.1 整体架构

```mermaid
graph TB
    subgraph "Application Layer"
        Client[Client Application]
    end

    subgraph "Mooncake Store"
        subgraph "Client Service"
            CS[Client Service]
            MC[Master Client]
        end

        subgraph "File Storage Layer"
            FS[FileStorage]
            CB[Client Buffer<br/>Allocator]
        end

        subgraph "Storage Backend"
            SBI[StorageBackendInterface]
            BSB[BucketStorageBackend]
            FPK[FilePerKeyBackend]
            OAS[OffsetAllocatorBackend]
        end

        subgraph "Master Service"
            MS[Master Service]
        end
    end

    subgraph "Storage"
        DISK[(Local Disk)]
    end

    Client --> CS
    CS --> MC
    CS --> FS
    FS --> CB
    FS --> SBI
    SBI --> BSB
    SBI --> FPK
    SBI --> OAS
    BSB --> DISK
    FPK --> DISK
    OAS --> DISK
    MC --> MS
```

### 2.2 核心组件职责

| 组件 | 职责 |
|-----|------|
| **FileStorage** | 文件存储管理器，协调心跳、数据卸载和加载操作 |
| **StorageBackend** | 存储后端抽象层，提供统一的数据读写接口 |
| **BucketStorageBackend** | 桶存储后端，将多个对象打包成桶存储，提高 I/O 效率 |
| **Client Buffer Allocator** | 客户端缓冲区分配器，管理数据读取时的临时内存 |
| **Master Service** | 元数据管理服务，跟踪对象位置和状态 |

## 3. 存储后端类型

Mooncake 支持三种存储后端，通过 `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR` 环境变量配置：

```mermaid
graph LR
    subgraph "Storage Backend Types"
        A[BucketStorageBackend<br/>bucket_storage_backend]
        B[FilePerKeyBackend<br/>file_per_key_storage_backend]
        C[OffsetAllocatorBackend<br/>offset_allocator_storage_backend]
    end

    A --> |"多个对象打包成桶<br/>适合小对象"| D[高 I/O 效率]
    B --> |"每个对象独立文件<br/>适合大对象"| E[简单直接]
    C --> |"预分配偏移量<br/>适合固定大小"| F[低碎片]
```

### 3.1 BucketStorageBackend（推荐）

将多个对象打包成一个"桶"（Bucket）进行存储：

- **优势**：减少文件数量，提高小对象存储效率
- **适用场景**：大量小对象的 KV 缓存
- **限制**：单个桶有键数量和大小限制

### 3.2 FilePerKeyBackend

每个对象对应一个独立文件：

- **优势**：实现简单，适合大对象
- **劣势**：大量小文件影响文件系统性能

### 3.3 OffsetAllocatorBackend

使用预分配的偏移量管理存储空间：

- **优势**：减少碎片，空间利用率高
- **适用场景**：对象大小相对固定的场景

## 4. 数据流程

### 4.1 Offload（数据卸载）流程

```mermaid
sequenceDiagram
    participant FS as FileStorage
    participant MS as Master Service
    participant SB as StorageBackend
    participant Disk as Local Disk

    Note over FS: Heartbeat Thread (周期性执行)

    FS->>MS: OffloadObjectHeartbeat(enable_offloading)
    MS-->>FS: 返回待卸载对象列表<br/>{key: size}

    loop 每批待卸载对象
        FS->>FS: BatchQuerySegmentSlices()<br/>获取内存中的数据切片
        FS->>SB: BatchOffload(batch_object)
        SB->>SB: BuildBucket()<br/>构建桶数据结构
        SB->>Disk: WriteBucket()<br/>写入磁盘
        SB-->>FS: 返回元数据
        FS->>MS: NotifyOffloadSuccess(keys, metadatas)
        MS-->>FS: 确认
    end
```

### 4.2 Load（数据加载）流程

```mermaid
sequenceDiagram
    participant Client as Client Application
    participant CS as Client Service
    participant FS as FileStorage
    participant SB as StorageBackend
    participant Disk as Local Disk

    Client->>CS: BatchGet(keys, sizes)
    CS->>FS: BatchGet(keys, sizes)

    FS->>FS: AllocateBatch()<br/>从 Client Buffer 分配内存

    FS->>SB: BatchLoad(batch_object)
    SB->>Disk: 读取桶数据
    Disk-->>SB: 返回数据
    SB-->>FS: 加载完成

    FS-->>CS: BatchGetResult{batch_id, pointers}
    CS-->>Client: 返回数据指针

    Note over Client: 使用数据...

    Client->>CS: ReleaseBuffer(batch_id)
    CS->>FS: ReleaseBuffer(batch_id)
    FS->>FS: 释放缓冲区
```

### 4.3 心跳与状态同步

```mermaid
sequenceDiagram
    participant FS as FileStorage
    participant MS as Master Service
    participant SB as StorageBackend

    Note over FS: 每 heartbeat_interval_seconds 秒执行

    FS->>SB: IsEnableOffloading()
    SB-->>FS: 返回是否允许卸载

    alt 允许卸载
        FS->>MS: OffloadObjectHeartbeat(true)
        MS-->>FS: 返回待卸载对象
        FS->>FS: OffloadObjects()
    else 不允许卸载
        FS->>MS: OffloadObjectHeartbeat(false)
        MS-->>FS: 空列表
    end
```

## 5. 配置参数详解

### 5.1 存储后端配置

| 环境变量 | 默认值 | 说明 |
|---------|-------|------|
| `MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR` | `bucket_storage_backend` | 存储后端类型 |
| `MOONCAKE_OFFLOAD_FILE_STORAGE_PATH` | `/data/file_storage` | 数据存储路径 |
| `MOONCAKE_OFFLOAD_FSDIR` | - | FilePerKey 后端目录名 |

### 5.2 容量限制配置

| 环境变量 | 默认值 | 说明 |
|---------|-------|------|
| `MOONCAKE_OFFLOAD_BUCKET_KEYS_LIMIT` | `500` | 单桶最大键数量 |
| `MOONCAKE_OFFLOAD_BUCKET_SIZE_LIMIT_BYTES` | `256MB` | 单桶最大大小 |
| `MOONCAKE_OFFLOAD_TOTAL_KEYS_LIMIT` | `10,000,000` | 全局最大键数量 |
| `MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES` | `2TB` | 全局最大存储大小 |

### 5.3 缓冲区配置

| 环境变量 | 默认值 | 说明 |
|---------|-------|------|
| `MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES` | `~1.2GB` | 本地缓冲区大小 |

### 5.4 心跳与 GC 配置

| 环境变量 | 默认值 | 说明 |
|---------|-------|------|
| `MOONCAKE_OFFLOAD_HEARTBEAT_INTERVAL_SECONDS` | `10` | 心跳间隔（秒） |
| `MOONCAKE_OFFLOAD_CLIENT_BUFFER_GC_INTERVAL_SECONDS` | `1` | 缓冲区 GC 间隔（秒） |
| `MOONCAKE_OFFLOAD_CLIENT_BUFFER_GC_TTL_MS` | `5000` | 缓冲区租约超时（毫秒） |

### 5.5 其他配置

| 环境变量 | 默认值 | 说明 |
|---------|-------|------|
| `MOONCAKE_USE_URING` | `false` | 是否使用 io_uring |
| `MOONCAKE_SCANMETA_ITERATOR_KEYS_LIMIT` | `20000` | 扫描元数据迭代器键限制 |

## 6. 核心数据结构

### 6.1 BucketMetadata

```cpp
struct BucketMetadata {
    int64_t meta_size;           // 元数据大小
    int64_t data_size;           // 数据大小
    std::vector<std::string> keys;           // 键列表
    std::vector<BucketObjectMetadata> metadatas;  // 对象元数据列表
    std::atomic<int32_t> inflight_reads_;    // 正在进行的读操作计数
};
```

### 6.2 BucketObjectMetadata

```cpp
struct BucketObjectMetadata {
    int64_t offset;    // 数据在桶中的偏移量
    int64_t key_size;  // 键大小
    int64_t data_size; // 数据大小
};
```

### 6.3 StorageObjectMetadata

```cpp
struct StorageObjectMetadata {
    int64_t bucket_id;    // 所属桶 ID
    int64_t offset;       // 桶内偏移
    int64_t key_size;     // 键大小
    int64_t data_size;    // 数据大小
    std::string transport_endpoint;  // 传输端点地址
};
```

## 7. 关键机制

### 7.1 桶分组策略

```mermaid
graph TB
    subgraph "待卸载对象"
        K1[Key1: 1MB]
        K2[Key2: 2MB]
        K3[Key3: 0.5MB]
        K4[Key4: 3MB]
        K5[Key5: 1MB]
    end

    subgraph "分组逻辑"
        GP[GroupOffloadingKeysByBucket]
        GP --> |"检查 bucket_keys_limit"| C1[键数量检查]
        GP --> |"检查 bucket_size_limit"| C2[大小检查]
    end

    subgraph "生成的桶"
        B1[Bucket 1<br/>Keys: K1, K2, K3<br/>Size: 3.5MB]
        B2[Bucket 2<br/>Keys: K4, K5<br/>Size: 4MB]
    end

    K1 & K2 & K3 & K4 & K5 --> GP
    GP --> B1
    GP --> B2
```

### 7.2 容量控制

```mermaid
flowchart TD
    A[IsEnableOffloading?] --> B{检查限制}

    B --> |"total_keys + bucket_keys_limit <= total_keys_limit"| C[键数量检查通过]
    B --> |"total_size + bucket_size_limit <= total_size_limit"| D[大小检查通过]

    C --> E{两个检查都通过?}
    D --> E

    E --> |是| F[返回 true<br/>允许卸载]
    E --> |否| G[返回 false<br/>禁止卸载]

    G --> H[触发 KEYS_ULTRA_LIMIT 错误]
```

### 7.3 客户端缓冲区管理

```mermaid
flowchart TB
    subgraph "Client Buffer Lifecycle"
        A[BatchGet 请求] --> B[AllocateBatch]
        B --> C[分配对齐内存<br/>4096 字节对齐]
        C --> D[设置租约超时<br/>lease_timeout = now + gc_ttl_ms]
        D --> E[返回 batch_id 和指针]

        E --> F[使用数据]

        F --> G{释放方式}
        G --> |主动释放| H[ReleaseBuffer]
        G --> |超时释放| I[GC Thread 自动清理]

        H --> J[释放内存]
        I --> J
    end

    subgraph "GC Thread"
        T[定时检查<br/>每 gc_interval_seconds]
        T --> K{检查租约}
        K --> |超时| L[释放 batch]
        K --> |未超时| M[跳过]
    end
```

### 7.4 安全读取保护（BucketReadGuard）

```mermaid
sequenceDiagram
    participant BL as BatchLoad
    participant RG as BucketReadGuard
    participant Bucket as BucketMetadata
    participant Disk as Disk I/O

    BL->>RG: 创建 Guard(bucket)
    RG->>Bucket: inflight_reads_++

    BL->>Disk: 执行磁盘读取

    Note over Disk: I/O 操作进行中...

    Disk-->>BL: 读取完成
    BL->>RG: Guard 析构
    RG->>Bucket: inflight_reads_--

    Note over Bucket: 当 inflight_reads_ == 0<br/>时才能安全删除桶
```

## 8. 初始化流程

```mermaid
flowchart TD
    A[FileStorage 构造] --> B[读取环境变量配置]
    B --> C[验证配置]

    C --> D[创建 StorageBackend]
    D --> E[注册本地内存]

    E --> F[Init StorageBackend]
    F --> G[ScanMeta 扫描已有数据]

    G --> H[NotifyOffloadSuccess<br/>通知 Master 已有对象]

    H --> I[启动 Heartbeat 线程]
    I --> J[启动 GC 线程]

    J --> K[初始化完成]
```

## 9. 错误处理

### 9.1 错误码

| 错误码 | 说明 |
|-------|------|
| `UNABLE_OFFLOAD` | Offload 功能未启用 |
| `UNABLE_OFFLOADING` | 无法执行卸载操作 |
| `KEYS_ULTRA_LIMIT` | 键数量超过全局限制 |
| `KEYS_EXCEED_BUCKET_LIMIT` | 单桶键数量超限 |
| `OBJECT_ALREADY_EXISTS` | 对象已存在（重复键） |
| `OBJECT_NOT_FOUND` | 对象未找到 |
| `BUCKET_NOT_FOUND` | 桶未找到 |

### 9.2 重复键处理

```mermaid
flowchart TD
    A[BatchOffload] --> B[写入桶文件]
    B --> C[获取排他锁]

    C --> D{检查重复键}
    D --> |发现重复| E[释放锁]
    E --> F[CleanupOrphanedBucket<br/>清理孤立桶文件]
    F --> G[返回 OBJECT_ALREADY_EXISTS]

    D --> |无重复| H[提交到内存映射]
    H --> I[返回成功]
```

## 10. 性能优化建议

### 10.1 缓冲区大小调优

```
MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES = 预期并发读取量 × 1.5
```

### 10.2 桶大小调优

```
MOONCAKE_OFFLOAD_BUCKET_KEYS_LIMIT = 根据对象平均大小调整
MOONCAKE_OFFLOAD_BUCKET_SIZE_LIMIT_BYTES = 磁盘顺序写入最佳块大小
```

### 10.3 心跳间隔调优

```
MOONCAKE_OFFLOAD_HEARTBEAT_INTERVAL_SECONDS = 数据变化频率 / 10
```

## 11. 监控指标

建议监控以下关键指标：

- `total_keys`：当前存储的总键数量
- `total_size`：当前存储的总数据大小
- `inflight_reads`：正在进行的读操作数量
- `client_buffer_usage`：客户端缓冲区使用率
- `offload_latency`：卸载操作延迟
- `load_latency`：加载操作延迟
