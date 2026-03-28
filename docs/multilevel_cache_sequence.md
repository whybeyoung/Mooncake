# Mooncake-Store 多级缓存技术方案

## 一、总述

Mooncake-Store 实现了 **L1 内存缓存 + L2 磁盘缓存** 的两级缓存架构。以内存缓存为主体提供高性能低延迟的数据读写，以磁盘缓存为兜底提供数据持久化与冷数据回退能力。两级缓存通过统一的 `Replica::Descriptor`（`std::variant<MemoryDescriptor, DiskDescriptor>`）对上层透明。

---

## 二、架构全景

### 2.1 系统整体架构

```mermaid
graph TB
    subgraph ClientSide["Client 侧"]
        App["Application"]
        Client["Client"]
        TxSubmitter["TransferSubmitter"]
        MemPool["MemcpyWorkerPool<br/>(1 thread)"]
        FilePool["FilereadWorkerPool<br/>(10 threads)"]
        WritePool["write_thread_pool_<br/>(2 threads)"]
        SB["StorageBackend"]
        Disk[("Local Disk / 3FS")]
    end

    subgraph MasterSide["Master 侧"]
        MasterSvc["MasterService"]
        SegMgr["SegmentManager"]
        Alloc["BufferAllocator<br/>(CacheLib / Offset)"]
        Metadata["MetadataShard × 1024<br/>ObjectMetadata"]
        Eviction["EvictionStrategy<br/>(LRU / FIFO)"]
        GC["GC Thread"]
    end

    subgraph TransferLayer["传输层"]
        TE["TransferEngine<br/>(RDMA / TCP)"]
    end

    subgraph RemoteNode["远程 Buffer Node"]
        RemoteMem[("Remote DRAM")]
    end

    App -->|"Get / Put"| Client
    Client -->|"RPC"| MasterSvc
    Client --> TxSubmitter
    TxSubmitter --> MemPool
    TxSubmitter --> FilePool
    TxSubmitter --> TE
    Client --> WritePool
    WritePool --> SB
    SB --> Disk
    FilePool --> SB
    TE --> RemoteMem

    MasterSvc --> SegMgr
    SegMgr --> Alloc
    MasterSvc --> Metadata
    MasterSvc --> GC
    GC --> Eviction

    style ClientSide fill:#e3f2fd,stroke:#1565c0
    style MasterSide fill:#fce4ec,stroke:#c62828
    style TransferLayer fill:#fff8e1,stroke:#f57f17
    style RemoteNode fill:#e8f5e9,stroke:#2e7d32
```

### 2.2 数据结构关系

```mermaid
classDiagram
    class Client {
        -TransferEngine transfer_engine_
        -MasterClient master_client_
        -TransferSubmitter transfer_submitter_
        -StorageBackend storage_backend_
        -ThreadPool write_thread_pool_
        +Get(key, slices)
        +Put(key, slices, config)
        +Query(key)
        +Remove(key)
    }

    class MasterService {
        -MetadataShard metadata_shards_[1024]
        -SegmentManager segment_manager_
        -AllocationStrategy allocation_strategy_
        +PutStart(key, lengths, config)
        +PutEnd(key)
        +GetReplicaList(key)
        +Remove(key)
    }

    class ObjectMetadata {
        +vector~Replica~ replicas
        +size_t size
        +time_point lease_timeout
        +optional~time_point~ soft_pin_timeout
        +double heat_score
        +GrantLease(ttl, soft_ttl)
        +IsLeaseExpired() bool
        +IsSoftPinned() bool
    }

    class Replica {
        -vector~AllocatedBuffer~ buffers_
        -ReplicaStatus status_
        +get_descriptor() Descriptor
        +mark_complete()
    }

    class ReplicaDescriptor {
        +variant descriptor_variant
        +ReplicaStatus status
        +is_memory_replica() bool
    }

    class MemoryDescriptor {
        +vector~BufferDescriptor~ buffer_descriptors
    }

    class DiskDescriptor {
        +string file_path
        +uint64_t file_size
    }

    class TransferSubmitter {
        -MemcpyWorkerPool memcpy_pool_
        -FilereadWorkerPool fileread_pool_
        -TransferEngine engine_
        +submit(replica, slices, op) TransferFuture
        -selectStrategy() TransferStrategy
        -isLocalTransfer() bool
    }

    class StorageBackend {
        -string root_dir_
        -string fsdir_
        +StoreObject(key, data)
        +LoadObject(path, slices, len)
        +Querykey(key) optional
        +RemoveFile(key)
    }

    Client --> MasterService : RPC
    Client --> TransferSubmitter : owns
    Client --> StorageBackend : owns
    MasterService "1" *-- "*" ObjectMetadata : metadata_shards_
    ObjectMetadata "1" *-- "1..*" Replica : replicas
    Replica ..> ReplicaDescriptor : produces
    ReplicaDescriptor --> MemoryDescriptor : variant-A
    ReplicaDescriptor --> DiskDescriptor : variant-B
    TransferSubmitter ..> ReplicaDescriptor : dispatches by type
    StorageBackend ..> DiskDescriptor : produces
```

---

## 三、核心流程

### 3.1 Put 操作时序图

```mermaid
sequenceDiagram
    autonumber
    participant App as Application
    participant Client as Client
    participant MC as MasterClient
    participant MS as MasterService
    participant Seg as SegmentManager
    participant TxSub as TransferSubmitter
    participant TE as TransferEngine
    participant Mcp as MemcpyWorkerPool
    participant WTP as write_thread_pool_
    participant SB as StorageBackend
    participant Disk as Disk/3FS

    App ->> Client: Put(key, slices, config)

    rect rgb(235, 245, 255)
        Note over Client, MS: Phase 1 — PutStart：Master 分配内存
        Client ->> MC: PutStart(key, slice_lengths, config)
        MC ->> MS: RPC
        MS ->> MS: getShardIndex(key) → lock shard

        alt key 已存在
            MS -->> MC: OBJECT_ALREADY_EXISTS
            MC -->> Client: error
            Client -->> App: return OK（幂等）
        else key 不存在
            MS ->> Seg: getAllocatorAccess()
            Seg ->> Seg: AllocationStrategy::select → allocate(size)

            alt 分配成功
                Seg -->> MS: AllocatedBuffer handles
                MS ->> MS: 创建 ObjectMetadata + Replica(INITIALIZED)
                MS ->> MS: GrantLease(ttl, soft_ttl)
                MS -->> MC: vector~Replica::Descriptor~
                MC -->> Client: replica descriptors
            else 内存不足 → 触发驱逐
                MS ->> MS: need_eviction_ = true
                MS -->> MC: NO_AVAILABLE_HANDLE
                MC -->> Client: error
                Client -->> App: return error
            end
        end
    end

    rect rgb(255, 248, 235)
        Note over Client, TE: Phase 2 — Transfer：写入数据到内存
        loop 对于每个 replica descriptor
            Client ->> TxSub: submit(replica, slices, WRITE)
            TxSub ->> TxSub: is_memory_replica() → selectStrategy()

            alt LOCAL_MEMCPY（本地 segment）
                TxSub ->> Mcp: submitTask(MemcpyTask)
                Mcp ->> Mcp: memcpy(dest, src, size)
                Mcp -->> TxSub: 完成
            else TRANSFER_ENGINE（远程 segment）
                TxSub ->> TE: allocBatchID + submitTransfer
                TE ->> TE: RDMA WRITE / TCP WRITE
                TE -->> TxSub: TransferFuture
            end
            TxSub -->> Client: TransferFuture
            Client ->> Client: future.get()
        end

        alt 任一 replica 传输失败
            Client ->> MC: PutRevoke(key)
            MC ->> MS: 释放 buffer + 删除元数据
            Client -->> App: return TRANSFER_FAIL
        end
    end

    rect rgb(235, 255, 240)
        Note over Client, MS: Phase 3 — PutEnd：提交完成
        Client ->> MC: PutEnd(key)
        MC ->> MS: RPC
        MS ->> MS: Replica status → COMPLETE
        MS -->> MC: OK
        MC -->> Client: OK
    end

    rect rgb(255, 243, 235)
        Note over Client, Disk: Phase 4 — L2 异步持久化（不阻塞返回）
        Client ->> WTP: enqueue(lambda)
        Client -->> App: return OK
        Note right of WTP: 异步执行 ↓
        WTP ->> SB: StoreObject(key, value)
        SB ->> SB: SanitizeKey → ResolvePath
        SB ->> Disk: write file
        Disk -->> SB: OK
    end
```

### 3.2 Get 操作时序图

```mermaid
sequenceDiagram
    autonumber
    participant App as Application
    participant Client as Client
    participant MC as MasterClient
    participant MS as MasterService
    participant TxSub as TransferSubmitter
    participant TE as TransferEngine
    participant Mcp as MemcpyWorkerPool
    participant FRP as FilereadWorkerPool
    participant SB as StorageBackend
    participant Disk as Disk/3FS

    App ->> Client: Get(key, slices)

    rect rgb(235, 245, 255)
        Note over Client, SB: Phase 1 — Query：定位数据所在层级
        Client ->> MC: GetReplicaList(key)
        MC ->> MS: RPC
        MS ->> MS: MetadataAccessor(key)

        alt L1 命中（内存缓存有数据）
            MS ->> MS: GrantLease 续约
            MS -->> MC: Replica::Descriptor (MemoryDescriptor)
            MC -->> Client: replica list
        else L1 未命中
            MS -->> MC: OBJECT_NOT_FOUND
            MC -->> Client: error

            alt storage_backend_ 可用
                Client ->> SB: Querykey(key)
                SB ->> Disk: stat file
                alt L2 命中（磁盘有数据）
                    Disk -->> SB: file exists
                    SB -->> Client: Replica::Descriptor (DiskDescriptor)
                else L2 也未命中
                    SB -->> Client: nullopt
                    Client -->> App: OBJECT_NOT_FOUND
                end
            else storage_backend_ 不可用
                Client -->> App: OBJECT_NOT_FOUND
            end
        end
    end

    rect rgb(255, 248, 235)
        Note over Client, Client: Phase 2 — 选择最优副本
        Client ->> Client: FindFirstCompleteReplica(replica_list)
        Note right of Client: 遍历 replica_list，<br/>返回第一个 status == COMPLETE 的

        alt 无 COMPLETE 副本
            Client -->> App: INVALID_REPLICA
        end
    end

    rect rgb(235, 255, 240)
        Note over Client, Disk: Phase 3 — Transfer：根据副本类型读取数据

        alt A: 内存副本 + 本地 segment
            Client ->> TxSub: submit(replica, slices, READ)
            TxSub ->> Mcp: submitTask
            Mcp ->> Mcp: memcpy(user_buf, cache_buf, size)
            Mcp -->> Client: TransferFuture → OK

        else B: 内存副本 + 远程 segment
            Client ->> TxSub: submit(replica, slices, READ)
            TxSub ->> TE: allocBatchID + submitTransfer
            TE ->> TE: RDMA READ / TCP READ
            TE -->> Client: TransferFuture → OK

        else C: 磁盘副本
            Client ->> TxSub: submit(replica, slices, READ)
            TxSub ->> FRP: submitTask(FilereadTask)
            FRP ->> SB: LoadObject(path, slices, size)
            SB ->> Disk: read file
            Disk -->> SB: data
            SB -->> FRP: OK
            FRP -->> Client: TransferFuture → OK
        end
    end

    alt 传输成功
        Client -->> App: OK（数据已在 slices 中）
    else 传输失败
        Client -->> App: TRANSFER_FAIL
    end
```

### 3.3 BatchPut 操作时序图

```mermaid
sequenceDiagram
    autonumber
    participant App as Application
    participant Client as Client
    participant MC as MasterClient
    participant MS as MasterService
    participant TxSub as TransferSubmitter
    participant WTP as write_thread_pool_
    participant SB as StorageBackend

    App ->> Client: BatchPut(keys[], batched_slices[], config)

    rect rgb(235, 245, 255)
        Note over Client, MS: Step 1 — BatchPutStart
        Client ->> Client: CreatePutOperations(keys, slices)
        Client ->> MC: BatchPutStart(keys, slice_lengths, config)
        MC ->> MS: RPC (批量)
        MS -->> MC: vector~Replica::Descriptor~ per key
        MC -->> Client: responses
        Client ->> Client: 逐个匹配 ops[i].replicas = responses[i]
    end

    rect rgb(255, 248, 235)
        Note over Client, TxSub: Step 2 — SubmitTransfers（并行提交）
        loop 对每个未失败的 op
            Client ->> TxSub: submit(replica, slices, WRITE)
            TxSub -->> Client: TransferFuture
            Client ->> Client: ops[i].pending_transfers.push(future)
        end
    end

    rect rgb(245, 245, 255)
        Note over Client, Client: Step 3 — WaitForTransfers
        loop 对每个 op
            Client ->> Client: future.get() for all replicas
            alt 全部成功
                Note right of Client: 继续
            else 任一失败
                Client ->> Client: op.SetError(TRANSFER_FAIL)
            end
        end
    end

    rect rgb(235, 255, 240)
        Note over Client, MS: Step 4 — FinalizeBatchPut
        Client ->> MC: BatchPutEnd(successful_keys)
        MC ->> MS: RPC
        MS -->> MC: OK per key
        Client ->> MC: BatchPutRevoke(failed_keys)
        MC ->> MS: RPC（释放空间）
    end

    rect rgb(255, 243, 235)
        Note over Client, SB: Step 5 — BatchPutToLocalFile（异步 L2 写入）
        loop 对每个成功的 op
            Client ->> WTP: enqueue(StoreObject)
            WTP ->> SB: StoreObject(key, value)
        end
    end

    Client -->> App: results[] per key
```

---

## 四、决策与策略

### 4.1 缓存层级选择决策流程

```mermaid
flowchart TD
    Start(["Get(key) 开始"]) --> QueryMaster["Master.GetReplicaList(key)"]

    QueryMaster --> MasterHit{L1 命中?}

    MasterHit -->|Yes| HasComplete{有 COMPLETE<br/>副本?}
    MasterHit -->|No| HasBackend{StorageBackend<br/>已初始化?}

    HasBackend -->|No| NotFound["返回 OBJECT_NOT_FOUND"]
    HasBackend -->|Yes| DiskQuery["StorageBackend.Querykey(key)"]

    DiskQuery --> DiskHit{L2 命中?}
    DiskHit -->|No| NotFound
    DiskHit -->|Yes| FileRead["FILE_READ<br/>FilereadWorkerPool"]

    HasComplete -->|No| NotFound2["返回 INVALID_REPLICA"]
    HasComplete -->|Yes| CheckType{副本类型?}

    CheckType -->|MemoryDescriptor| CheckLocal{segment_name_<br/>== local_hostname_?}
    CheckType -->|DiskDescriptor| FileRead

    CheckLocal -->|"Yes & memcpy_enabled"| Memcpy["LOCAL_MEMCPY<br/>MemcpyWorkerPool"]
    CheckLocal -->|No| RDMA["TRANSFER_ENGINE<br/>RDMA / TCP"]

    Memcpy --> ReturnOK(["返回数据 OK"])
    RDMA --> ReturnOK
    FileRead --> ReturnOK

    style Start fill:#e1f5fe,stroke:#0277bd
    style ReturnOK fill:#c8e6c9,stroke:#2e7d32
    style NotFound fill:#ffcdd2,stroke:#c62828
    style NotFound2 fill:#ffcdd2,stroke:#c62828
    style Memcpy fill:#fff9c4,stroke:#f9a825
    style RDMA fill:#fff9c4,stroke:#f9a825
    style FileRead fill:#ffe0b2,stroke:#e65100
```

### 4.2 Put 操作中 L1/L2 写入关系

```mermaid
flowchart LR
    subgraph Critical["关键路径（同步，阻塞返回）"]
        direction TB
        P1["PutStart<br/>Master 分配内存"] --> P2["Transfer<br/>写入 L1 内存缓存"]
        P2 --> P3["PutEnd<br/>标记 COMPLETE"]
    end

    subgraph Async["非关键路径（异步，不阻塞）"]
        direction TB
        P4["PutToLocalFile"] --> P5["write_thread_pool_<br/>数据拷贝"]
        P5 --> P6["StorageBackend<br/>写入 L2 磁盘"]
    end

    P3 --> Return(["return OK"])
    P3 -.->|"fire & forget"| P4

    style Critical fill:#e3f2fd,stroke:#1565c0
    style Async fill:#fff3e0,stroke:#e65100
    style Return fill:#c8e6c9,stroke:#2e7d32
```

### 4.3 GC / 驱逐策略流程

```mermaid
flowchart TD
    GCThread(["GC Thread<br/>(100ms 轮询)"]) --> CheckQueue{gc_queue_<br/>有到期任务?}
    CheckQueue -->|Yes| DeleteKey["删除到期 key"]
    CheckQueue -->|No| CheckEvict{need_eviction_<br/>== true?}

    CheckEvict -->|No| Sleep["sleep 100ms"]
    CheckEvict -->|Yes| Pass1

    subgraph TwoPassEviction["两轮驱逐"]
        direction TB
        Pass1["第一轮：仅驱逐非 soft_pin 对象<br/>目标：eviction_ratio (10%)"]
        Pass1 --> CheckEnough{达到目标?}
        CheckEnough -->|Yes| Done["驱逐完成"]
        CheckEnough -->|No| Pass2["第二轮：允许驱逐 soft_pin 对象<br/>(若 allow_evict_soft_pinned)"]
        Pass2 --> Done
    end

    Done --> ResetFlag["need_eviction_ = false"]
    ResetFlag --> Sleep
    Sleep --> GCThread

    DeleteKey --> Sleep

    style GCThread fill:#e1f5fe
    style TwoPassEviction fill:#fce4ec,stroke:#c62828
    style Done fill:#c8e6c9
```

### 4.4 传输策略对比

```mermaid
graph LR
    subgraph S1["LOCAL_MEMCPY"]
        direction TB
        S1a["条件：本地 segment<br/>+ MC_STORE_MEMCPY=true"]
        S1b["实现：MemcpyWorkerPool<br/>(1 worker thread)"]
        S1c["性能：内存带宽<br/>~100 GB/s"]
        S1a --> S1b --> S1c
    end

    subgraph S2["TRANSFER_ENGINE"]
        direction TB
        S2a["条件：远程 segment<br/>（默认路径）"]
        S2b["实现：TransferEngine<br/>RDMA / TCP"]
        S2c["性能：网络带宽<br/>~25-100 Gbps"]
        S2a --> S2b --> S2c
    end

    subgraph S3["FILE_READ"]
        direction TB
        S3a["条件：DiskDescriptor<br/>（L2 回退）"]
        S3b["实现：FilereadWorkerPool<br/>(10 worker threads)"]
        S3c["性能：SSD 带宽<br/>~3-7 GB/s"]
        S3a --> S3b --> S3c
    end

    style S1 fill:#c8e6c9,stroke:#2e7d32
    style S2 fill:#fff9c4,stroke:#f9a825
    style S3 fill:#ffe0b2,stroke:#e65100
```

---

## 五、配置与开关

```mermaid
flowchart TD
    subgraph EnvVars["环境变量"]
        E1["MOONCAKE_STORAGE_ROOT_DIR<br/>L2 根目录，空=关闭 L2"]
        E2["MC_STORE_MEMCPY<br/>本地 memcpy 开关，默认 false"]
        E3["USE_3FS (编译宏)<br/>是否支持 3FS 文件系统"]
    end

    subgraph MasterCfg["Master 配置"]
        M1["cluster_id<br/>磁盘子目录 moon_$cluster_id"]
        M2["enable_dynamic_ttl<br/>动态 TTL"]
        M3["eviction_ratio / watermark<br/>驱逐比例"]
        M4["allocation_strategy<br/>random / weighted_random"]
    end

    E1 -->|非空| L2On["L2 磁盘缓存开启"]
    E1 -->|空| L2Off["仅 L1 内存缓存"]
    E2 -->|true| MemcpyOn["本地传输用 memcpy"]
    E2 -->|false| MemcpyOff["本地传输也走 TransferEngine"]

    style EnvVars fill:#e3f2fd,stroke:#1565c0
    style MasterCfg fill:#fce4ec,stroke:#c62828
    style L2On fill:#c8e6c9
    style L2Off fill:#fff9c4
```

---

## 六、错误处理全景

### 6.1 Put 错误处理

```mermaid
flowchart TD
    PutStart{PutStart 结果} -->|OBJECT_ALREADY_EXISTS| Idempotent["return OK（幂等）"]
    PutStart -->|NO_AVAILABLE_HANDLE| AllocFail["return error<br/>Master 触发驱逐"]
    PutStart -->|OK| Transfer

    Transfer{Transfer 结果} -->|失败| Revoke["PutRevoke(key)"]
    Revoke --> RevokeResult{Revoke 成功?}
    RevokeResult -->|Yes| RetErr1["return TRANSFER_FAIL<br/>空间已释放"]
    RevokeResult -->|No| RetErr2["return error<br/>空间泄漏风险"]
    Transfer -->|成功| PutEnd

    PutEnd{PutEnd 结果} -->|OK| L2Write["异步写 L2"]
    PutEnd -->|失败| RetErr3["return error<br/>Replica 状态不一致"]

    L2Write --> L2Result{L2 写入结果}
    L2Result -->|成功| AllOK(["return OK"])
    L2Result -->|失败| AllOK
    Note right of L2Result: L2 失败不影响返回值<br/>仅丢失持久化副本

    style Idempotent fill:#c8e6c9
    style AllOK fill:#c8e6c9
    style AllocFail fill:#ffcdd2
    style RetErr1 fill:#ffcdd2
    style RetErr2 fill:#ffcdd2
    style RetErr3 fill:#ffcdd2
```

### 6.2 Get 错误处理

```mermaid
flowchart TD
    Query{Query Master} -->|OK| FindReplica
    Query -->|OBJECT_NOT_FOUND| HasSB{StorageBackend?}

    HasSB -->|有| DiskQuery{Querykey(key)}
    HasSB -->|无| Err1["OBJECT_NOT_FOUND"]

    DiskQuery -->|命中| FindReplica
    DiskQuery -->|未命中| Err1

    FindReplica{FindFirstComplete<br/>Replica} -->|找到| DoTransfer
    FindReplica -->|未找到| Err2["INVALID_REPLICA"]

    DoTransfer{Transfer 结果} -->|OK| RetOK(["return OK"])
    DoTransfer -->|失败| Err3["TRANSFER_FAIL"]

    style RetOK fill:#c8e6c9
    style Err1 fill:#ffcdd2
    style Err2 fill:#ffcdd2
    style Err3 fill:#ffcdd2
```

---

## 七、现状评估

### 7.1 数据流向总图

```mermaid
flowchart LR
    subgraph Write["写入路径"]
        direction TB
        W1["Client.Put"] -->|同步| W2["L1 内存缓存<br/>(Master 管理)"]
        W1 -->|"异步 fire&forget"| W3["L2 磁盘缓存<br/>(Client 本地)"]
    end

    subgraph Read["读取路径"]
        direction TB
        R1["Client.Get"] -->|"优先查"| R2["L1 内存缓存"]
        R2 -->|miss| R3["L2 磁盘缓存"]
    end

    subgraph NotSupported["当前不支持"]
        direction TB
        N1["L2 → L1 回填<br/>(promotion)"]
        N2["L2 容量管理<br/>& 淘汰"]
        N3["跨 Client 的<br/>L2 共享"]
    end

    W2 -.->|"无自动同步"| W3
    R3 -.->|"无 promotion"| R2

    style Write fill:#e3f2fd,stroke:#1565c0
    style Read fill:#e8f5e9,stroke:#2e7d32
    style NotSupported fill:#ffebee,stroke:#c62828
```

### 7.2 L1 vs L2 特性对比

```mermaid
graph TB
    subgraph L1["L1 内存缓存"]
        direction TB
        L1a["管理者: MasterService（集中式）"]
        L1b["存储: 分布式 DRAM segments"]
        L1c["分配: CacheLib slab / Offset allocator"]
        L1d["淘汰: GC + LRU/FIFO + TTL lease"]
        L1e["传输: memcpy / RDMA / TCP"]
        L1f["容量: 有上限，主动驱逐"]
        L1g["可见性: 全局（所有 Client 共享）"]
    end

    subgraph L2["L2 磁盘缓存"]
        direction TB
        L2a["管理者: Client（各自独立）"]
        L2b["存储: 本地 SSD / 3FS"]
        L2c["分配: 文件系统目录结构"]
        L2d["淘汰: 无（无限增长）"]
        L2e["传输: FilereadWorkerPool"]
        L2f["容量: 无上限管理"]
        L2g["可见性: 仅本 Client"]
    end

    style L1 fill:#e3f2fd,stroke:#1565c0
    style L2 fill:#fff3e0,stroke:#e65100
```

### 7.3 回退触发条件分析

```mermaid
flowchart TD
    subgraph Triggers["会触发 L2 回退的场景"]
        T1["Master 返回<br/>OBJECT_NOT_FOUND"]
        T2["Lease 过期被 GC 清理<br/>L1 无数据"]
        T3["Segment unmount<br/>导致 stale handle 清理"]
    end

    subgraph NoTrigger["不会触发 L2 回退的场景"]
        N1["Master 返回 replica<br/>但 status != COMPLETE"]
        N2["Transfer 传输失败<br/>（已拿到 replica descriptor）"]
        N3["Master RPC 超时<br/>（非 NOT_FOUND 错误）"]
    end

    T1 --> Fallback["→ StorageBackend.Querykey()"]
    T2 --> Fallback
    T3 --> Fallback

    N1 --> NoFallback["→ 直接返回 INVALID_REPLICA"]
    N2 --> NoFallback2["→ 直接返回 TRANSFER_FAIL"]
    N3 --> NoFallback3["→ 直接返回 RPC_FAIL"]

    style Triggers fill:#e8f5e9,stroke:#2e7d32
    style NoTrigger fill:#ffebee,stroke:#c62828
    style Fallback fill:#c8e6c9
```
```mermaid
graph TB                                     
      subgraph GPU["GPU VRAM"]
          KV["KVCache Tensor"]                                                                                                                              
      end                                                                                                                                                   
                                                                                                                                                            
      subgraph DRAM["Memory — L1 缓存（Master 集中管理）"]                                                                                                  
          direction LR                                                                                                                                    
          LocalMem["本机 DRAM<br/>Segment"]
          RemoteMem["远程节点 DRAM<br/>Segment"]
      end                                                                                                                                                   
  
      subgraph SSD["SSD — L2 缓存（Client 本地）"]                                                                                                          
          DiskFile["StorageBackend<br/>文件: moon_cluster/&lt;key&gt;"]                                                                                   
      end                                                                                                                                                   
  
      %% ======== Put 路径 ========                                                                                                                         
      KV ==>|"① Put: Client 注册<br/>slices 指向 GPU 内存"| LocalMem                                                                                      
      KV ==>|"② Put: RDMA WRITE<br/>写入远程节点"| RemoteMem
      LocalMem -.->|"③ 异步下沉<br/>write_thread_pool_<br/>(fire & forget)"| DiskFile
      RemoteMem -.->|"③ 异步下沉<br/>Client 侧数据拷贝后写盘"| DiskFile                                                                                     
                                                                                                                                                            
      %% ======== Get 路径 ========                                                                                                                         
      LocalMem ==>|"④ Get 命中 L1<br/>memcpy 直拷"| KV                                                                                                      
      RemoteMem ==>|"⑤ Get 命中 L1<br/>RDMA READ"| KV                                                                                                       
      DiskFile ==>|"⑥ Get L1 miss<br/>回退读盘<br/>FilereadWorkerPool"| KV                                                                                  
                                                                                                                                                            
      %% 样式                                                                                                                                               
      style GPU fill:#c8e6c9,stroke:#2e7d32,stroke-width:2px                                                                                                
      style DRAM fill:#e3f2fd,stroke:#1565c0,stroke-width:2px                                                                                               
      style SSD fill:#fff3e0,stroke:#e65100,stroke-width:2px 
```