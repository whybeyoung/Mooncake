# Group Eviction 实现方案（仅内存层，固定 group key 版）

## Context

当前 mooncake-store 的驱逐机制（`BatchEvict`）是按单个 key 独立驱逐的。现在只考虑内存缓存这一层，不考虑 L3 / SSD offload / disk replica。

你的 key 形式已经明确为：

```text
{groupId}_pprank0_pprank1_xxx
```

这意味着 group 不需要运行时做“最长相同前缀”推断，而是可以直接从 key 本身稳定计算出来。

本版方案基于三个约束：

- 只修改内存层 `BatchEvict`
- 不修改 `ReplicateConfig`，也不引入显式 `group_id`
- group 直接由 key 格式计算，不能在 eviction 热路径做全表前缀匹配

另外，这个特性必须是**完全可开关**的：

- 部署时静态指定是否开启
- 关闭时不维护 group 索引
- 关闭时 `BatchEvict` 完全走现有逻辑
- 关闭时系统行为应与当前版本一致

## 需求确认

- 作用范围：只处理 memory eviction
- 选择语义：单 key 被现有算法选中后，带走同 group 的其他 key
- group 规则：从 key 中提取固定 group key
- 强制语义：group peer 可以绕过 lease / soft_pin / `refcnt`

---

## 核心设计

### Group 规则

定义：

- `group key` = 第一个 `'_'` 之前的前缀

例子：

- `session123_pprank0_pprank1_layer0` -> group key = `session123`
- `abc_pprank3_pprank7_kvchunk42` -> group key = `abc`
- `foo_bar` -> group key = `foo`

如果 key 中不存在 `'_'`，则认为该 key 不参与 group eviction，只按单 key 处理。

### 为什么这个规则更适合实现

因为 group key 可以仅靠当前 key 自身计算：

- 计算 group key：`O(L)`
- 插入索引：接近 `O(1)`
- 驱逐时查组成员：`O(G)`
  - `G` = group 大小

相比“最长相同前缀”方案：

- 不需要全表扫描
- 不依赖当前已有 key 集
- 不受插入顺序影响
- 语义稳定得多

---

## 修改文件清单

| 文件 | 变更 |
|------|------|
| `mooncake-store/include/master_config.h` | 增加 `enable_group_eviction` 开关 |
| `mooncake-store/include/master_service.h` | 新增 group 索引结构与 helper |
| `mooncake-store/src/master_service.cpp` | `PutStart` / `PutRevoke` / `Remove` / `RemoveAll` / `BatchEvict` 的 group 逻辑 |
| `mooncake-store/src/master.cpp` | 新增 `--enable_group_eviction` gflag |
| `mooncake-store/tests/group_eviction_test.cpp` | 新增测试文件 |
| `mooncake-store/CMakeLists.txt` | 注册新测试 |

---

## Step 1: Config 层仅增加 enable 开关

**文件**: `mooncake-store/include/master_config.h`

只增加一个开关：

```cpp
bool enable_group_eviction = false;
```

需要透传到：

- `MasterConfig`
- `MasterServiceSupervisorConfig`
- `WrappedMasterServiceConfig`
- `MasterServiceConfig`
- `MasterServiceConfigBuilder`

如果测试依赖 `InProcMasterConfigBuilder`，也需要补这个字段。

说明：

- 不增加 `group_separator`
- 不修改 `ReplicateConfig`
- 这是部署期静态配置，不要求运行时动态切换

建议语义：

- master 启动时读取 `enable_group_eviction`
- `MasterService` 构造后，该值固定不变
- 不提供运行中热更新，否则索引初始化/清理会复杂很多

## Step 2: MasterService 增加 group 索引

**文件**: `mooncake-store/include/master_service.h`

新增成员：

```cpp
const bool enable_group_eviction_;

static constexpr size_t kNumGroupShards = 64;

struct GroupShard {
    mutable SharedMutex mutex;
    std::unordered_map<std::string, std::string> key_to_group;
    std::unordered_map<std::string, std::unordered_set<std::string>> group_to_keys;
};

std::array<GroupShard, kNumGroupShards> group_shards_;
```

含义：

- `key_to_group`: key 当前归属的 group key
- `group_to_keys`: group key 对应的全部成员 key

辅助函数：

```cpp
size_t getGroupShardIndex(const std::string& group_key) const;

std::optional<std::string> ExtractGroupKey(const std::string& key) const;

void RegisterKeyToGroupIndex(const std::string& key);

void RemoveKeyFromGroupIndex(const std::string& key);

std::vector<std::string> GetGroupMembers(const std::string& key) const;
```

建议语义：

- `ExtractGroupKey(key)`：
  - 查找第一个 `'_'`
  - 若存在且位置大于 0，返回 `key.substr(0, pos)`
  - 否则返回空
- `RegisterKeyToGroupIndex(key)`：
  - 计算 group key
  - 若有值，则更新 `key_to_group` 和 `group_to_keys`
- `GetGroupMembers(key)`：
  - 先查 `key_to_group`
  - 再查对应 `group_to_keys`

分片建议：

- 按 `group_key` hash 到 `group_shards_`
- 同一 group 的元数据固定落到同一个 group shard，便于维护

实现要求：

- 当 `enable_group_eviction_ == false` 时，这些索引结构可以存在，但不参与任何维护逻辑
- 所有索引 helper 的第一行都应快速判断该开关，关闭时直接返回

## Step 3: 写路径维护 group 索引

**文件**: `mooncake-store/src/master_service.cpp`

需要在以下场景维护索引：

- `PutStart` 成功插入 metadata 后：`RegisterKeyToGroupIndex(key)`
- `PutRevoke` 在 metadata 被彻底 erase 前：`RemoveKeyFromGroupIndex(key)`
- `Remove` erase 前：`RemoveKeyFromGroupIndex(key)`
- `RemoveByRegex` erase 前：`RemoveKeyFromGroupIndex(key)`
- `RemoveAll` 对每个被删除对象逐个移除索引，不能直接清空全部 group_shards_

说明：

- 因为这次是显式维护索引，所以删除路径必须同步维护
- `RemoveAll(false)` 只删除部分对象，直接清空索引会把剩余对象的 group 信息弄坏
- 当 `enable_group_eviction_ == false` 时，上述索引维护逻辑全部跳过

## Step 4: BatchEvict 保留 seed 选择逻辑，只在执行阶段扩组

**文件**: `mooncake-store/src/master_service.cpp`

这是核心改动。

### 4.1 保留现有 seed 选择逻辑

当 `enable_group_eviction_ == false` 时，完全保留现有逻辑。

当 `enable_group_eviction_ == true` 时：

- 第一轮和第二轮仍沿用当前单 key eviction 选择算法
- `can_evict_replicas` 仍然只约束 seed key
- seed key 一旦被选中，再去 group 索引里查询同组成员

这意味着：

- 开关关闭：`BatchEvict` 的时间复杂度、行为、统计口径都和现在一致
- 开关开启：才引入 group 扩组逻辑和索引查询

### 4.2 扩组

当某个 seed key 被选中后：

```cpp
auto group_keys = GetGroupMembers(seed_key);
```

规则：

- 如果 key 不在 `key_to_group` 中，则只驱逐自己
- 如果 key 有 group，则把同组全部 key 加入 `eviction_keys`
- 用 `std::unordered_set<std::string>` 去重

### 4.3 执行驱逐

对 `eviction_keys` 中的 key：

- 若 key 是 seed key：
  - 沿用现有逻辑
  - 删除 `completed && refcnt == 0` 的 memory replica
- 若 key 是 group peer：
  - 强制删除所有 `completed` 的 memory replica
  - 不检查 lease
  - 不检查 soft pin
  - 不检查 `refcnt`
  - 不删除 processing replica

删除完成后：

- 若 metadata 失效，则 erase，并从 group 索引删除
- `evicted_count` 仍按“实际删掉 memory replica 的对象数”累计

### 4.4 复杂度

有索引后：

- seed 选择复杂度基本不变
- 扩组是：
  - `O(1)` 计算 group key 或查询 `key_to_group`
  - `O(G)` 获取组内成员
- 不再有运行时全表前缀匹配

这对 eviction 热路径是可接受的。

## Step 5: 不处理 disk eviction / offload / snapshot

本版不做以下内容：

- 不改 `EvictDiskReplica`
- 不改 `PushOffloadingQueue`
- 不改 `OffloadObjectHeartbeat`
- 不改 snapshot 序列化 / 反序列化

原因：

- 你已经明确不需要关心 L3 / offload
- group 不进入协议层和持久化层

## Step 6: master.cpp gflag

**文件**: `mooncake-store/src/master.cpp`

新增：

```cpp
DEFINE_bool(enable_group_eviction, false, "Enable group eviction for memory cache");
```

并确保：

- config 文件支持这个项
- 命令行覆盖逻辑支持这个项
- 启动日志打印该项

建议部署语义：

- 这是静态部署参数
- 一套 deployment 开或者不开，由启动参数或配置文件决定
- 不要求运行过程中切换

## Step 7: 测试

**新文件**: `mooncake-store/tests/group_eviction_test.cpp`

至少覆盖以下用例：

| 测试用例 | 验证内容 |
|---------|---------|
| `GroupEviction_Disabled` | 关闭 group eviction 时行为不变 |
| `GroupEviction_Disabled_NoIndexMaintenance` | 关闭时不维护 group 索引，写路径无额外副作用 |
| `ExtractGroupKey_Basic` | `session123_pprank0_pprank1_xxx` 提取出 `session123` |
| `ExtractGroupKey_FirstUnderscoreOnly` | `foo_bar_baz` 提取出 `foo` |
| `ExtractGroupKey_NoUnderscore` | 不含 `'_'` 的 key 不参与 group |
| `GroupIndex_RegisterAndRemove` | Put/Remove 后索引正确维护 |
| `GroupEviction_Basic` | 选中一个 seed key 时，同组一起驱逐 |
| `GroupEviction_CrossShard` | group 成员跨 metadata shard 时仍能一起驱逐 |
| `GroupEviction_ForcePastRefcnt` | peer `refcnt > 0` 仍会被带走 |
| `GroupEviction_ForcePastSoftPin` | peer soft pin 不阻止驱逐 |
| `GroupEviction_ForcePastLease` | peer lease 未过期也会被带走 |
| `GroupEviction_SkipProcessingPeer` | peer 只有 processing replica 时不会被删坏 |
| `RemoveAll_DoesNotCorruptRemainingGroups` | `RemoveAll(false)` 删除部分对象后，剩余 group 索引仍正确 |

---

## 实现顺序

1. Step 1: 增加 `enable_group_eviction`
2. Step 2: 定义 group 索引与 `ExtractGroupKey`
3. Step 3: 在写路径维护索引
4. Step 4: 修改 `BatchEvict`，在执行阶段查索引扩组
5. Step 6: 接入 gflag
6. Step 7: 补测试

## 风险与注意事项

1. **key 格式必须稳定**：该方案强依赖“group key 在第一个 `'_'` 之前”这一约定；一旦 key 格式变化，group 提取规则也要跟着改。
2. **会主动破坏 refcnt 保护**：peer 即使正在被引用，也会被强制驱逐；后台操作失败属于接受范围。
3. **资源抖动仍然存在**：seed key 按单 key 选，但实际可能带走整个 group，释放量会超 target。
4. **索引一致性必须做好**：所有 erase 路径都要维护 `group_to_keys`，否则会出现脏成员。
5. **不开启时必须零行为变化**：这是一个部署期开关，关闭后应尽量接近零性能损耗、零语义变化。
