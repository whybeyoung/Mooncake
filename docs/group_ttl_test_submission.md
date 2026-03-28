# Group TTL 功能提测说明

**文档版本**: 1.0  
**适用分支**: `dev/mooncake-store-test`（以实际提测分支为准）  
**日期**: 2026-03-28

---

## 一、功能概述

**Group TTL** 在 Mooncake Master 侧为「逻辑分组」提供 **租约联动续约** 与 **批量驱逐联动**，解决多 Key 同属一条业务语义（例如同一 session 下多个 tensor 分片）时，仅访问「热 Key」导致同组「冷 Key」因未触发 `GetReplicaList` 而提前过期被驱逐的问题。

- **关闭** `enable_group_ttl` 时：行为与改造前一致，每个 Key 独立续约、独立按租约参与驱逐。
- **开启**后：由 Key 命名规则推导分组；访问组内任一 Key 时，为整组续约；批量驱逐选中组内某一 Key 时，在同一次驱逐流程中联动清理同组其他 Key 的内存副本（peer 侧使用更激进的副本擦除策略）。

---

## 二、代码结构梳理

| 模块 | 路径 | 职责 |
|------|------|------|
| 开关与配置 | `mooncake-store/include/master_config.h`、`mooncake-store/src/master.cpp` | `enable_group_ttl`（默认 `false`），可由 gflags `--enable_group_ttl` 与配置文件覆盖 |
| 核心逻辑 | `mooncake-store/src/master_service.cpp` | 分组解析、组索引、续约、批量驱逐分支 |
| 指标 | `mooncake-store/src/master_metric_manager.cpp`、`include/master_metric_manager.h` | Group TTL 相关 Prometheus 指标 |
| 单元测试 | `mooncake-store/tests/master_service_test.cpp` | `GroupTTL*`、`ExtractGroupKey*` 等用例 |
| 集成脚本 | `scripts/test_group_ttl_with_curl.sh` | 基于 HTTP Store + Master metrics 的端到端校验 |

### 2.1 分组 ID（Group Key）解析

实现：`MasterService::ExtractGroupKey`（约 ```179:191:mooncake-store/src/master_service.cpp```）

- 仅当 **`enable_group_ttl == true`** 时参与分组。
- 若 Key 中 **不存在** `_`，或 **`_` 在首字符**，则 **不分组**（返回空），该 Key 仅自身参与原 TTL 逻辑。
- 否则：**分组 ID = 第一个 `_` 之前的子串**（与 `_` 之后有多少段无关）。例如 `foo_bar_baz` → 组 `foo`。

### 2.2 组索引（内存结构）

实现：`RegisterKeyToGroupIndex` / `RemoveKeyFromGroupIndex` / `GetGroupMembers`（约 ```193:254:mooncake-store/src/master_service.cpp```）

- 使用 **`group_key` 的哈希 % 64** 映射到 `group_shards_` 分片，降低锁竞争（见 `master_service.h` 中 `kNumGroupShards = 64`）。
- 维护 `key_to_group` 与 `group_to_keys`；同一 `group_key` 下所有已注册 Key 视为同组。
- 对象创建/删除路径中维护索引；Snapshot 恢复后调用 `RebuildGroupIndex()` 全量重建。

### 2.3 读路径：整组续约

实现：`GetReplicaList` 在为本 Key `GrantLease` 之后调用 `GrantLeaseToGroup(key)`（约 ```755:795:mooncake-store/src/master_service.cpp```）。

- `GrantLeaseToGroup`：遍历 `GetGroupMembers` 返回的成员，对 **存在的** 元数据调用 `GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_)`。
- 对 **除触发 Key 以外** 的成员续约次数计入 `master_group_ttl_collateral_lease_renewals_total`（「连带续约」指标）。
- 若某 peer Key 已被删除，跳过即可，**不导致**本次 Get 失败（见单测 `GroupTTLMissingPeerDoesNotBreakRenewal`）。

### 2.4 驱逐路径：组内联动驱逐

实现：`BatchEvict` 在 `enable_group_ttl_` 为真时走独立分支（约 ```2972:3210:mooncake-store/src/master_service.cpp```）。

- 先按原策略选出 **seed**（过期且可驱逐的候选 Key）。
- 对每个 seed：若属于某组，则对 seed 使用常规 `evict_seed_replicas`；对 **同组其他 Key** 使用 `evict_group_peer_replicas`（对已完成内存副本更激进，不受 refcnt==0 限制），实现「一带多」清内存。
- 联动驱逐的 peer 数量计入 `master_group_ttl_collateral_evictions_total`。

### 2.5 指标与日志摘要

| 指标名 | 含义 |
|--------|------|
| `master_group_ttl_group_count` | 当前 Master 跟踪的 **逻辑组数**（Gauge） |
| `master_group_ttl_collateral_lease_renewals_total` | 因组内访问而 **额外续约** 的 Key 次数（Counter） |
| `master_group_ttl_collateral_evictions_total` | 因组驱逐而 **连带驱逐** 的 Key 次数（Counter） |

Master 日志 summary 中含 `GroupTTL: groups=..., collateral_lease_keys=..., collateral_evicted_keys=...` 片段便于排查。

---

## 三、公规（对外约定）

以下为测试与业务接入需共同遵守的约定，避免「未分组」或「错误合组」。

### 3.1 Key 命名公规

1. **需要参与同一 Group TTL 的 Key**：必须包含 **至少一个** `_`，且 **首字符不能为 `_`**。  
   - 组 ID = **第一个 `_` 之前**的字符串（全量作为组名，不再拆分）。
2. **不需要分组的 Key**：不要使用 `_`，或接受「仅第一个 `_` 前为组名」带来的分组行为。
3. **脚本/客户端注意**：若业务 Key 本身含 `_`，同一逻辑会话下所有应联动的 Key 应共享 **相同的前缀（至第一个 `_`）**。测试脚本 `test_group_ttl_with_curl.sh` 会 **去掉 Key 中所有 `_` 再构造前缀**，用于避免与组规则冲突；业务侧应理解真实 Key 与组 ID 的对应关系。

### 3.2 配置公规

| 配置项 | 说明 |
|--------|------|
| `--enable_group_ttl` / `enable_group_ttl` | 总开关，默认 **false** |
| `default_kv_lease_ttl` | 组内续约使用的租约 TTL（与单 Key 一致，单位以 Master 配置为准，通常为毫秒） |
| `default_kv_soft_pin_ttl` | 组内续约同时刷新 soft pin 时间（与单 Key 一致） |

### 3.3 行为公规

1. **续约**：仅在 **`GetReplicaList` 成功路径** 上触发整组续约（与单 Key 续约同一入口语义）。
2. **不保证**「租约到期即立刻删除」：过期对象成为驱逐候选，是否立即消失取决于内存压力与驱逐策略；测试脚本中已说明（见脚本 `Important` 段）。
3. **关闭 Group TTL**：同前缀多 Key **不会** 建立组索引；批量驱逐 **不会** 联动驱逐同组 peer（单测 `GroupTTLDisabledBatchEvictKeepsGroupPeer`）。

---

## 四、提测范围与验收标准

### 4.1 提测范围

- Master：**Group TTL** 开关、组索引、读路径续约、批量驱逐联动、Snapshot 恢复后组索引重建。
- **不包含**：客户端 API 新接口；Group TTL 仅影响 Master 元数据与驱逐行为，客户端仍按原协议 `GetReplicaList` / Put 流程访问。

### 4.2 自动化验收

1. **单元测试**（建议在提测环境执行）  
   - 构建并运行 `mooncake-store` 相关测试，重点：`GroupTTL*`、`ExtractGroupKey*`。  
   - 命令示例（以工程实际 target 为准）：  
     `ctest -R MasterService --output-on-failure` 或等价 gtest 过滤。

2. **脚本验收**（可选，依赖 `mooncake_store_test` + Master metrics HTTP）  
   - 启动 Master：`mooncake_master --enable_group_ttl=true --default_kv_lease_ttl=3000`（TTL 以毫秒计，与脚本参数 `--ttl-seconds` 对应关系按部署约定）。  
   - 执行：`bash scripts/test_group_ttl_with_curl.sh`  
   - **通过条件**：  
     - 仅并发 GET「热 Key」多轮后，同组「冷 Key」仍存在于 `get_all_keys` 列表；  
     - `master_group_ttl_collateral_lease_renewals_total` 增量 **≥** `group_count * rounds`（脚本内计算）。

### 4.3 手工与监控验收（建议）

- 开启前后对比：冷 Key 在只读热 Key 场景下是否仍被误驱逐（业务可构造 `prefix_hot` / `prefix_cold`）。  
- Grafana / Prometheus：`master_group_ttl_*` 三类指标随读写与驱逐变化合理；日志 summary 中 GroupTTL 字段可核对。

### 4.4 已知风险与回归关注点

- **Key 设计**：误用 `_` 可能导致无关 Key 被划入同一组，带来额外续约或联动驱逐；需在业务侧规范命名。  
- **性能**：热点 Key 会触发同组多 Key 的元数据续约，组越大，单次 `GetReplicaList` 额外开销越大。  
- **兼容性**：默认关闭，线上需显式开启；开启后驱逐行为与「仅按单 Key」不同，需压测观察驱逐分布。

---

## 五、提测检查清单

| 序号 | 检查项 | 结果（打勾） |
|------|--------|----------------|
| 1 | `enable_group_ttl=false` 时行为与旧版一致（含批量驱逐不联动） | |
| 2 | `enable_group_ttl=true` 时，同组仅访问部分 Key 可续约整组 | |
| 3 | 无 `_` 的 Key 不参与分组 | |
| 4 | Snapshot 恢复后组索引与指标一致（若有 Snapshot 场景） | |
| 5 | Prometheus 三类 Group TTL 指标可抓取且语义正确 | |
| 6 | 单元测试与（若部署）curl 脚本通过 | |

---

## 六、参考文件路径（研发自测）

- 核心实现：`mooncake-store/src/master_service.cpp`（`ExtractGroupKey`、`GrantLeaseToGroup`、`BatchEvict` 中 `group_ttl` 分支）
- 测试：`mooncake-store/tests/master_service_test.cpp`
- 端到端脚本：`scripts/test_group_ttl_with_curl.sh`

---

*本文档由研发根据当前代码整理，用于内部提测与评审；若实现变更，以仓库最新代码与测试为准。*
