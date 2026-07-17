# Redis DAO 层集群兼容修复计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Redis DAO 层中逻辑关联的多 key 在 Cluster 模式下因缺少 hash tag 而分布到不同节点，导致 SCAN 漏数据、双 key 操作跨节点无原子性的问题。

**Architecture:** 对需要共址的逻辑关联 key 添加 Redis Cluster hash tag（`{...}`），确保它们 CRC16 到同一 slot→同一节点。改动范围：UnackedPush 双 key（sorted set + hash 索引）、Presence device key（3 处写入 + 2 处 SCAN）。

**Tech Stack:** C++17, sw::redis++

**Spec:** `docs/superpowers/specs/2026-05-18-cache-infrastructure-redesign.md` §1.2（注意：spec 中"不加 hash tag"的结论基于"所有操作都是单 key"的前提，此前提对 UnackedPush 和 Presence device keys 不成立）

---

## Bug 根因分析

### Bug 1: UnackedPush 双 key 无 hash tag

`push()` / `ack()` / `bump_score()` 每个方法操作两个逻辑耦合的 key：

```
im:unack:{uid}:{device_id}      → Sorted Set（重传队列，按时间戳排序）
im:unack:idx:{uid}:{device_id}  → Hash（O(1) 索引，user_seq→payload_b64）
```

在 Cluster 模式下，这两个 key 的 CRC16 不同 → 落在**不同节点**：
- `push()`: ZADD(节点A) + HSET(节点B) + EXPIRE(A) + EXPIRE(B) — 4 次跨节点命令，任一失败产生孤立数据
- `ack()`: HGET(节点B) → ZREM(节点A) + HDEL(节点B) — 读 B 写 A+B，非原子
- `bump_score()`: 循环 HGET(B) → ZADD(A) — N 次跨节点乒乓

### Bug 2: Presence device key 无 hash tag

同一用户的设备 key 分布在 Cluster 不同节点，SCAN 遍历所有节点才能找全：

```
写入（3 处）:
  push_server.h:471            → im:presence:device:{uid}:{did}
  data_redis.hpp:986           → im:presence:device:{uid}:{device_id}
  
SCAN 读取（2 处）:
  presence_server.h:52         → SCAN im:presence:device:{uid}:*
  data_redis.hpp:997           → SCAN im:presence:device:{uid}:*
```

不加 hash tag 时，`uid123:devA` 和 `uid123:devB` 可能在不同节点。sw::redis++ 的 `RedisCluster::scan()` 虽内部遍历所有 master 节点，但如果某节点不可达或重定向，部分设备会被漏掉。

---

## File Structure

```
common/dao/data_redis.hpp          — UnackedPush::key_for/idx_key_for 加 hash tag
                                      PresenceRedis::add_device/get_devices 加 hash tag
push/source/push_server.h          — _write_presence_online_ key 加 hash tag
presence/source/presence_server.h  — PresenceAggregator::aggregate SCAN pattern 加 hash tag
common/test/test_redis_cluster.cc  — 新增：hash tag 共址验证测试
```

---

### Task 1: UnackedPush 双 key 添加 hash tag

**Files:**
- Modify: `common/dao/data_redis.hpp:860-864`

**Why:** `key_for()` 和 `idx_key_for()` 生成的两个 key 在 Cluster 中必须落在同一 slot。使用 `{uid:device_id}` 作为 hash tag，精确共址 per-user-device 数据对，避免 per-user 热点。

- [ ] **Step 1: 修改 key_for 和 idx_key_for**

在 `common/dao/data_redis.hpp` 中，将 `UnackedPush` 类的两个静态方法替换为：

```cpp
// Before (lines 860-864):
static std::string key_for(const std::string &uid, const std::string &device_id) {
    return std::string(key::kUnacked) + uid + ":" + device_id;
}
static std::string idx_key_for(const std::string &uid, const std::string &device_id) {
    return std::string(key::kUnacked) + "idx:" + uid + ":" + device_id;
}

// After:
static std::string key_for(const std::string &uid, const std::string &device_id) {
    return std::string(key::kUnacked) + "{" + uid + ":" + device_id + "}";
}
static std::string idx_key_for(const std::string &uid, const std::string &device_id) {
    return std::string(key::kUnacked) + "idx:{" + uid + ":" + device_id + "}";
}
```

**效果验证**：
- `key_for("u1", "d1")` → `"im:unack:{u1:d1}"` → CRC16("u1:d1") = slot S
- `idx_key_for("u1", "d1")` → `"im:unack:idx:{u1:d1}"` → CRC16("u1:d1") = slot S ✅ 共址

**迁移注意**：旧格式 key（`im:unack:u1:d1`）在部署后变为孤儿数据，依赖 7 天 TTL 自动过期。部署时建议 flush 或接受短暂残留。

- [ ] **Step 2: 构建验证**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix(redis): add hash tag to UnackedPush dual keys for cluster co-location"
```

---

### Task 2: PresenceRedis device key 添加 hash tag

**Files:**
- Modify: `common/dao/data_redis.hpp:985-998`

**Why:** `add_device()` 写入和 `get_devices()` SCAN 必须在同一 hash slot 才能可靠查到全量设备。

- [ ] **Step 1: 修改 add_device 的 key 生成**

Replace `PresenceRedis::add_device()` (lines 985-989):

```cpp
// Before:
void add_device(const std::string &uid, const std::string &device_id) {
    auto k = std::string("im:presence:device:") + uid + ":" + device_id;
    _r->hset(k, "state", "ONLINE");
    _r->expire(k, std::chrono::seconds(120));
}

// After:
void add_device(const std::string &uid, const std::string &device_id) {
    auto k = std::string("im:presence:device:{") + uid + "}:" + device_id;
    _r->hset(k, "state", "ONLINE");
    _r->expire(k, std::chrono::seconds(120));
}
```

- [ ] **Step 2: 修改 get_devices 的 SCAN pattern**

Replace `PresenceRedis::get_devices()` (lines 992-1006):

```cpp
// Before:
std::vector<std::string> get_devices(const std::string &uid) {
    std::vector<std::string> out;
    auto cursor = 0ULL;
    while (true) {
        std::vector<std::string> batch;
        cursor = _r->scan(cursor, "im:presence:device:" + uid + ":*", 100,
                         std::back_inserter(batch));
        for (auto& k : batch) {
            auto pos = k.rfind(':');
            if (pos != std::string::npos) out.push_back(k.substr(pos + 1));
        }
        if (cursor == 0) break;
    }
    return out;
}

// After:
std::vector<std::string> get_devices(const std::string &uid) {
    std::vector<std::string> out;
    auto cursor = 0ULL;
    while (true) {
        std::vector<std::string> batch;
        cursor = _r->scan(cursor, "im:presence:device:{" + uid + "}:*", 100,
                         std::back_inserter(batch));
        for (auto& k : batch) {
            auto pos = k.rfind(':');
            if (pos != std::string::npos) out.push_back(k.substr(pos + 1));
        }
        if (cursor == 0) break;
    }
    return out;
}
```

**效果验证**：
- `add_device("u1", "d1")` → key `"im:presence:device:{u1}:d1"` → CRC16("u1")
- `add_device("u1", "d2")` → key `"im:presence:device:{u1}:d2"` → CRC16("u1") ✅ 同节点
- `get_devices("u1")` SCAN `"im:presence:device:{u1}:*"` → 在单节点命中全部设备 ✅

- [ ] **Step 3: 构建验证**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix(redis): add hash tag to PresenceRedis device keys for cluster SCAN reliability"
```

---

### Task 3: Push 服务 `_write_presence_online_` key 格式同步

**Files:**
- Modify: `push/source/push_server.h:471`

**Why:** push_server.h 直接拼 key 写入 presence device 数据，必须与 Task 2 的 hash tag 格式一致，否则 PresenceAggregator 的 SCAN 找不到 push_server 写入的设备。

- [ ] **Step 1: 修改 key 构造**

Replace line 471 in `push/source/push_server.h`:

```cpp
// Before:
std::string k = std::string("im:presence:device:") + uid + ":" + did;

// After:
std::string k = std::string("im:presence:device:{") + uid + "}:" + did;
```

- [ ] **Step 2: 构建验证**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add push/source/push_server.h
git commit -m "fix(redis): sync push _write_presence_online_ key format with hash tag"
```

---

### Task 4: Presence 服务 SCAN pattern 格式同步

**Files:**
- Modify: `presence/source/presence_server.h:52`

**Why:** `PresenceAggregator::aggregate()` 的 SCAN pattern 必须与写入方的 hash tag 格式一致。

- [ ] **Step 1: 修改 SCAN pattern**

Replace line 52 in `presence/source/presence_server.h`:

```cpp
// Before:
cursor, "im:presence:device:" + uid + ":*", 100,

// After:
cursor, "im:presence:device:{" + uid + "}:*", 100,
```

- [ ] **Step 2: 构建验证**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add presence/source/presence_server.h
git commit -m "fix(redis): sync presence aggregator SCAN pattern with hash tag"
```

---

### Task 5: 新增 Hash Tag 共址单元测试

**Files:**
- Create: `common/test/test_redis_hash_tag.cc`

**Why:** 验证 hash tag 确实将相关 key 锚定到同一 slot，防止未来修改破坏共址约束。

- [ ] **Step 1: 编写测试**

创建 `common/test/test_redis_hash_tag.cc`：

```cpp
#include "dao/data_redis.hpp"
#include <gtest/gtest.h>

// 验证 UnackedPush 双 key 共址
TEST(RedisHashTag, UnackedPushKeysSameSlot) {
    // 在 Redis Cluster 中，CRC16 of "{uid:did}" 决定 slot
    // hash tag 保证 key_for 和 idx_key_for 中的 hash 内容一致
    auto k1 = chatnow::UnackedPush::key_for("user123", "device456");
    auto k2 = chatnow::UnackedPush::idx_key_for("user123", "device456");

    // 提取 hash tag 内容（{...} 之间的部分）
    auto extract_tag = [](const std::string& s) -> std::string {
        auto l = s.find('{');
        auto r = s.find('}');
        if (l == std::string::npos || r == std::string::npos) return "";
        return s.substr(l + 1, r - l - 1);
    };

    EXPECT_EQ(extract_tag(k1), "user123:device456");
    EXPECT_EQ(extract_tag(k2), "user123:device456");
    // 两 key 的 hash tag 内容相同 → CRC16 相同 → 同一 slot
}

// 验证不同用户/设备的 UnackedPush key 有不同 hash tag
TEST(RedisHashTag, UnackedPushKeysDifferentTagPerDevice) {
    auto k1 = chatnow::UnackedPush::key_for("user1", "dev1");
    auto k2 = chatnow::UnackedPush::key_for("user1", "dev2");

    EXPECT_NE(k1, k2);
    // 不同 device 有不同 hash tag → 分布在集群中（避免 per-user 热点）
}

// 验证 Presence device key 中同一用户的设备共址
TEST(RedisHashTag, PresenceDeviceKeyHashTag) {
    // 模拟 add_device 生成的 key
    auto make_key = [](const std::string& uid, const std::string& did) {
        return std::string("im:presence:device:{") + uid + "}:" + did;
    };

    auto extract_tag = [](const std::string& s) -> std::string {
        auto l = s.find('{');
        auto r = s.find('}');
        if (l == std::string::npos || r == std::string::npos) return "";
        return s.substr(l + 1, r - l - 1);
    };

    auto k1 = make_key("user123", "devA");
    auto k2 = make_key("user123", "devB");
    auto k3 = make_key("user456", "devA");

    EXPECT_EQ(extract_tag(k1), "user123");
    EXPECT_EQ(extract_tag(k1), extract_tag(k2));   // 同用户 → 同 slot
    EXPECT_NE(extract_tag(k1), extract_tag(k3));   // 不同用户 → 不同 slot（正常分布）
}

// 验证 SCAN pattern 的 hash tag 与写入 key 一致
TEST(RedisHashTag, PresenceDeviceScanPatternMatches) {
    std::string uid = "user789";
    std::string scan_pattern = "im:presence:device:{" + uid + "}:*";
    std::string write_key = "im:presence:device:{" + uid + "}:devXYZ";

    // SCAN pattern 的前缀部分必须与写入 key 的前缀（在 hash tag 之前）一致
    EXPECT_EQ(scan_pattern.substr(0, scan_pattern.find('*')),
              write_key.substr(0, write_key.find('}') + 1) + ":");
}
```

- [ ] **Step 2: 注册测试到 CMake**

在 `common/test/CMakeLists.txt` 中添加：

```cmake
add_executable(test_redis_hash_tag test_redis_hash_tag.cc)
target_link_libraries(test_redis_hash_tag gtest gtest_main pthread)
add_test(NAME test_redis_hash_tag COMMAND test_redis_hash_tag)
```

（注：实际 CMakeLists.txt 配置需根据项目现有模式调整，可能在顶层 CMakeLists 中统一管理）

- [ ] **Step 3: 运行测试验证通过**

```bash
cd build && cmake .. && make test_redis_hash_tag && ./common/test/test_redis_hash_tag
```

Expected: 4 tests PASS

- [ ] **Step 4: Commit**

```bash
git add common/test/test_redis_hash_tag.cc common/test/CMakeLists.txt
git commit -m "test: add hash tag co-location verification for cluster-mode keys"
```

---

### Task 6: SCAN 集群模式警告注释

**Files:**
- Modify: `common/dao/data_redis.hpp:992-993`
- Modify: `presence/source/presence_server.h:46-47`
- Modify: `push/source/push_server.h` (如有 SCAN 使用处)

**Why:** 虽然 hash tag 确保同一用户设备在同一节点，但如果 sw::redis++ 的 `RedisCluster::scan()` 在有节点故障时行为异常，仍可能漏数据。添加注释标记已知依赖，方便未来排障。

- [ ] **Step 1: 在 PresenceRedis::get_devices 添加注释**

在 `data_redis.hpp:992` 的 `get_devices` 方法前添加：

```cpp
// NOTE: 依赖 hash tag {uid} 确保同用户所有 device key 在同一 Cluster 节点。
// 依赖 sw::redis++ RedisCluster::scan() 内部遍历所有 master 节点。
// 若未来出现 SCAN 漏设备问题，优先排查 hash tag 是否被破坏。
```

- [ ] **Step 2: 在 PresenceAggregator::aggregate 添加相同注释**

在 `presence_server.h:46` 的 SCAN 前添加相同注释。

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp presence/source/presence_server.h
git commit -m "docs: add cluster SCAN dependency notes for presence device keys"
```

---

## 影响范围总结

| 改动 | 文件 | 影响 |
|------|------|------|
| UnackedPush key 格式 | `data_redis.hpp` | 旧格式 key 成为孤儿，7 天 TTL 自动清理 |
| Presence device key 格式 | `data_redis.hpp`, `push_server.h`, `presence_server.h` | 旧格式 key 成为孤儿，120s TTL 自动清理 |
| SCAN pattern 格式 | `data_redis.hpp`, `presence_server.h` | 与写入 key 格式同步 |

**部署注意**：所有写入方和读取方必须**同时部署**。如果灰度发布，旧实例写旧格式 key、新实例扫描新格式 pattern，会出现短暂查不到设备的情况（影响窗口 = 120s TTL，自愈）。

---

## 不变更项（经分析无需修复）

| 项 | 分析 |
|----|------|
| `SeqGen::next_user_seq_batch()` pipeline 跨 slot | sw::redis++ `RedisCluster::pipeline()` 内部按节点拆分、合并结果，INCR 原子性由每个节点独立保证，无需 hash tag |
| `RedisClient::pipeline()` 返回类型 | 当前能编译说明 sw::redis++ 的两种 Pipeline 类型兼容（同接口或隐式转换），不需修改 |
| `JwtStore` 多 key | `revoke` / `put_active_refresh` / `rotate` 各自使用独立单 key，无共址需求 |
| `RedisMutex` / `WorkerIdAllocator` | 均单 key 操作，Lua 脚本只有单个 KEYS，Cluster 完全兼容 |

---

## Verification Checklist

### UnackedPush 共址
- [ ] `key_for("u1", "d1")` 和 `idx_key_for("u1", "d1")` 的 hash tag 内容相同（均为 `u1:d1`）
- [ ] `key_for("u1", "d1")` 和 `key_for("u1", "d2")` 的 hash tag 不同（分别为 `u1:d1` 和 `u1:d2`，正常分布）
- [ ] `push()` 调用后，`ack()` 能正确 HGET → ZREM + HDEL
- [ ] `bump_score()` 循环能正确 HGET → ZADD

### Presence device 共址
- [ ] push_server 写入 key 格式：`im:presence:device:{uid}:did`
- [ ] PresenceRedis::add_device 写入 key 格式：`im:presence:device:{uid}:device_id`
- [ ] PresenceRedis::get_devices SCAN pattern：`im:presence:device:{uid}:*`
- [ ] PresenceAggregator::aggregate SCAN pattern：`im:presence:device:{uid}:*`

### 全量构建
- [ ] `cd build && cmake .. && make -j4` 所有 target 编译通过
- [ ] `ctest --output-on-failure` 所有测试通过（含新增 test_redis_hash_tag）
