# ChatNow 缓存基础设施重构设计

> **日期**: 2026-05-18
> **基线**: 3.0-dev
> **范围**: Redis 集群化 / 多级缓存 / 分布式锁分层 / 缓存防护体系
> **关联**: `2026-05-13-cache-strategy-redesign.md`（数据层改进，本设计的基础设施层补充）

---

## 0. 设计目标

在不考虑老版本和客户端兼容性的前提下，将缓存基础设施从"单机 Redis + 单层缓存"升级到支持大规模 IM 场景的"Redis Cluster + 多级缓存 + 分层锁"架构。

### 0.1 目标规模

- DAU: 50 万+
- 峰值消息量: 5000+ msg/s
- 群规模: 最大 2000 人/群
- 服务实例: 每服务 2-8 实例

### 0.2 当前痛点（来自 review）

| # | 痛点 | 当前 | 目标 |
|---|------|------|------|
| 1 | 单 Redis 实例 | 无 HA，挂了全链路停摆 | Redis Cluster 3 主 3 从 |
| 2 | 无本地缓存 | OnlineRoute 每条消息 200 次 Redis HKEYS | 本地短 TTL 缓存，削减 90%+ Redis 读 |
| 3 | 分布式锁分散 | Outbox reaper 用 Lua CAS，Snowflake 用 etcd 租约，不一致 | 统一分层：etcd 管选举，Redis 管互斥 |
| 4 | 无防击穿 | 热点 key 过期 → N 个并发同时穿透 | InflightRegistry（已有设计）+ 本地缓存兜底 |
| 5 | TTL 雪崩 | 固定 TTL，集中过期 | 随机偏移 TTL（已有设计） |
| 6 | 缓存穿透 | 不存在的数据反复查 | 空值缓存（sentinel） |

---

## 1. Redis 集群化

### 1.1 拓扑：3 主 3 从 Cluster

```
                 ┌──────────────────────────────────────┐
                 │         Redis Cluster (6 nodes)       │
                 │                                      │
                 │  Master A (slot 0-5460)   ── Slave A │
                 │  Master B (slot 5461-10922) ── Slave B│
                 │  Master C (slot 10923-16383)── Slave C│
                 │                                      │
                 │  自动 failover: Sentinel 内置于       │
                 │  Redis Cluster (cluster-replica-no)   │
                 └──────────────────────────────────────┘
```

- **选型**: Redis 7.x Cluster 原生模式（内置 gossip + 自动 failover）
- **为什么不用 Codis/Proxy 方案**: 代理层多一跳延迟 + 单点瓶颈；原生 Cluster 的 smart client 自动重定向，延迟更低
- **为什么不用 Sentinel 主从**: Sentinel 只能管一个分片，无法横向扩展；Cluster 管 16384 个 slot，吞吐随节点数线性增长

### 1.2 分片策略

**不加 hash tag，自然 CRC16 分布**。

理由：
- 所有 Redis 操作都是单 key（INCR / SADD / HSET / ZADD / Lua EVAL），无跨 key 原子性需求
- `SeqGen::next_user_seq_batch` 的 pipeline INCR 由 sw::redis++ `RedisCluster` 自动按 slot 分组转发
- 不加 hash tag 避免热点——`im:seq:ssid:abc123` 和 `im:members:ssid:abc123` 落在不同节点

### 1.3 客户端改造

当前 `RedisClientFactory` 返回 `sw::redis::Redis`（单机客户端），新增 `RedisClusterFactory`：

```cpp
// common/dao/data_redis.hpp

class RedisClusterFactory {
public:
    static std::shared_ptr<sw::redis::RedisCluster> create(
        const std::vector<std::string> &seed_nodes,  // {"10.0.4.10:6379", "10.0.4.11:6379", ...}
        int pool_size = 16,                           // 每节点的连接池
        bool keep_alive = true)
    {
        sw::redis::ConnectionOptions copts;
        copts.connect_timeout = std::chrono::milliseconds(2000);
        copts.socket_timeout  = std::chrono::milliseconds(2000);
        copts.keep_alive = keep_alive;

        sw::redis::ConnectionPoolOptions popts;
        popts.size = pool_size;
        popts.wait_timeout = std::chrono::milliseconds(500);
        popts.connection_lifetime = std::chrono::minutes(30);

        return std::make_shared<sw::redis::RedisCluster>(copts, popts);
    }
};
```

**各服务使用策略**：

| 服务 | 客户端类型 | 原因 |
|------|-----------|------|
| Transmite | RedisCluster | SeqGen INCR + Members 读 + RateLimiter，高频 |
| Message | RedisCluster | SeqGen backfill + PushOutbox + ESOutbox |
| Push | RedisCluster | OnlineRoute + UnackedPush + Presence，高频 |
| ChatSession | RedisCluster | Members 写（add/remove）+ LastMessage |
| Identity | RedisCluster | Session + Codes + Status |
| Gateway | RedisCluster | Session 校验（仅读） |
| Media | RedisCluster | 限流 + 上传配额 |

### 1.4 配置变更

所有 `*.conf` 文件从单地址改为种子节点列表：

```
# 旧
-redis_host=10.0.4.10
-redis_port=6379

# 新
-redis_seeds=10.0.4.10:6379,10.0.4.11:6379,10.0.4.12:6379
-redis_pool_size=16
```

---

## 2. 多级缓存架构

### 2.1 分层模型

```
                         ┌──────────────────────┐
       get(key)          │   L1: 本地内存缓存    │  ~ns 级延迟
    ────────────────────▶│   tsl::hopscotch_map  │
                         │   + mutex + TTL       │
                         └──────────┬───────────┘
                                    │ miss
                                    ▼
                         ┌──────────────────────┐
                         │   L2: Redis Cluster   │  ~0.5ms 延迟
                         │   真相源 (SoT)        │
                         └──────────┬───────────┘
                                    │ miss（仅 Members/UserInfo）
                                    ▼
                         ┌──────────────────────┐
                         │   L3: RPC / DB        │  ~5-20ms 延迟
                         │   ChatSession RPC     │
                         │   或 MySQL            │
                         └──────────────────────┘
```

### 2.2 L1 本地缓存设计

不是所有 key 都需要本地缓存。需要本地缓存的判断标准：

| 数据 | 本地缓存 | TTL | 理由 |
|------|---------|-----|------|
| OnlineRoute (用户→设备→实例) | **是** | 1-3s | 每条群消息查 N 次，N=群成员数。1s 过期可削减 99% Redis 读 |
| Members (群成员列表) | **是** | 5-10s | 活跃群高频访问，成员变更低频。10s 过期 + 变更时主动失效 |
| UserInfo (用户昵称/头像) | **是** | 30-60s | 用户信息几乎不变，30s 过期可接受 |
| ChatSession list | **是** | 10-30s | 会话列表高频访问 |
| SeqGen (序号) | **否** | — | 必须强一致，INCR 走 Redis |
| RateLimiter (限流) | **否** | — | 必须跨实例共享计数 |
| UnackedPush (未 ACK) | **否** | — | 必须持久化，实例 crash 后重启仍需要 |
| PushOutbox / ESOutbox | **否** | — | 必须持久化到 Redis |
| Session (登录态) | **否** | — | 已由调用方做本地校验（JWT） |

### 2.3 LocalCache 通用组件

```cpp
// common/utils/local_cache.hpp
#pragma once

#include <shared_mutex>
#include <chrono>
#include <string>
#include <unordered_map>
#include <optional>

namespace chatnow {

template <typename V>
class LocalCache {
public:
    using ptr = std::shared_ptr<LocalCache<V>>;

    struct Entry {
        V value;
        std::chrono::steady_clock::time_point expires_at;
    };

    // size_hint: 预估 key 数量上限
    explicit LocalCache(size_t size_hint = 4096) {
        _map.reserve(size_hint);
    }

    std::optional<V> get(const std::string &key) {
        std::shared_lock lk(_mu);
        auto it = _map.find(key);
        if (it == _map.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() > it->second.expires_at) {
            // 过期不移除（lazy），由后续 set 覆盖
            return std::nullopt;
        }
        return it->second.value;
    }

    void set(const std::string &key, const V &value,
             std::chrono::seconds ttl) {
        std::unique_lock lk(_mu);
        _map[key] = {value, std::chrono::steady_clock::now() + ttl};
    }

    // CAS: 仅当 key 不存在时设置（防击穿用——第一个 miss 的请求 set，后续直接 get）
    bool set_if_absent(const std::string &key, const V &value,
                       std::chrono::seconds ttl) {
        std::unique_lock lk(_mu);
        auto it = _map.find(key);
        if (it != _map.end() &&
            std::chrono::steady_clock::now() <= it->second.expires_at) {
            return false;  // 已存在且未过期
        }
        _map[key] = {value, std::chrono::steady_clock::now() + ttl};
        return true;
    }

    void invalidate(const std::string &key) {
        std::unique_lock lk(_mu);
        _map.erase(key);
    }

    // 后台清理过期条目（可定期调用或在 size 超过阈值时触发）
    size_t evict_expired() {
        std::unique_lock lk(_mu);
        auto now = std::chrono::steady_clock::now();
        size_t removed = 0;
        for (auto it = _map.begin(); it != _map.end(); ) {
            if (now > it->second.expires_at) {
                it = _map.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    size_t size() const {
        std::shared_lock lk(_mu);
        return _map.size();
    }

private:
    mutable std::shared_mutex _mu;
    std::unordered_map<std::string, Entry> _map;
};

} // namespace chatnow
```

### 2.4 多级缓存读路径模板

以 OnlineRoute 为例，Push 服务中 `instances(uid)` 的读路径：

```
OnlineRouteService::instances(uid):
  ① local = _local_cache.get("route:" + uid)
     if local → return local

  ② redis = _redis_cluster.hgetall("im:online:" + uid)
     if redis not empty:
       _local_cache.set("route:" + uid, redis, TTL=2s)
       return redis

  ③ return {}  // 用户不在线
```

以 Members 为例，Transmite 中 `resolve_members(ssid)` 的读路径：

```
MembersResolver::resolve(ssid):
  ① local = _local_cache.get("members:" + ssid)
     if local → return local

  ② redis = _members_cache.list(ssid)
     if redis not empty:
       _local_cache.set("members:" + ssid, redis, TTL=8s)
       return redis

  ③ // 以下走现有 InflightRegistry + RPC 回填逻辑
```

### 2.5 本地缓存失效策略

| 触发方 | 操作 | 方法 |
|--------|------|------|
| ChatSession 加人/踢人 | 失效对应 ssid 的 Members 本地缓存 | **不需要主动失效**——5-10s TTL 自动过期。成员变更频率极低（< 0.01/s/群），10s 窗口可接受 |
| User 改资料 | 失效对应 uid 的 UserInfo 本地缓存 | 同上——30s TTL 自动过期 |
| Push 实例上线/下线 | OnlineRoute 由 Redis online key 的 TTL 控制 | 本地缓存 1-3s，Redis TTL 30s，心跳续期 |

**结论：本地缓存完全通过短 TTL 自愈，不需要跨服务失效通知。** 这避免引入 Pub/Sub 或 gRPC 通知的复杂性。

---

## 3. 分布式锁分层方案

### 3.1 分层决策

| 层级 | 技术 | 场景 | 持锁时间 | 一致性要求 |
|------|------|------|----------|-----------|
| **Tier 1: 选举锁** | etcd 租约 (Lease) | Outbox reaper 选举、Snowflake worker_id 分配 | 30-60s，持续续约 | 强一致（Raft） |
| **Tier 2: 短时互斥锁** | Redis Lua CAS | InflightRegistry 分布式版、cache warm 互斥、SeqGen backfill 协调 | 毫秒-秒级 | 最终一致可接受 |

### 3.2 Tier 1: etcd 选举锁

**为什么用 etcd？**
- 项目已经依赖 etcd 做服务发现（`registry_host=http://10.0.4.10:2379`），零新增组件
- etcd v3 的 Lease + Transaction 提供强一致 CAS（基于 Raft），不会出现脑裂双主
- 持锁时间 30-60s 的场景，etcd 的写延迟（~10ms）完全可接受

**统一选举锁组件**：

```cpp
// common/infra/leader_election.hpp
#pragma once

#include <etcd/Client.hpp>
#include <etcd/LeaseKeepAlive.hpp>
#include <functional>
#include <atomic>

namespace chatnow {

// etcd 选举锁：自动续约 + 租约丢失回调
class LeaderElection {
public:
    using ptr = std::shared_ptr<LeaderElection>;

    // on_acquired: 成为 leader 时回调
    // on_lost:     失去 leader 时回调（租约过期 / etcd 不可达）
    LeaderElection(
        std::shared_ptr<etcd::Client> etcd,
        const std::string &election_key,  // e.g. "/chatnow/push_outbox_reaper/leader"
        const std::string &instance_id,
        int lease_ttl_sec,
        std::function<void()> on_acquired,
        std::function<void()> on_lost);

    // 启动选举循环（blocking 方式在独立线程中运行）
    void start();
    void stop();

    bool is_leader() const { return _is_leader.load(); }

private:
    void campaign_loop_();  // Campaign → keepalive → watch
    // ...
};

} // namespace chatnow
```

**使用场景映射**：

| 场景 | election_key | lease_ttl |
|------|-------------|-----------|
| PushOutbox reaper | `/chatnow/reaper/push_outbox` | 30s |
| ESOutbox reaper | `/chatnow/reaper/es_outbox` | 30s |
| CrossInstanceOutbox reaper | `/chatnow/reaper/cross_outbox` | 30s |
| Snowflake worker_id | `/chatnow/snowflake/worker/{id}` | 60s |

### 3.3 Tier 2: Redis 短时互斥锁

用于高频、短时、可容忍偶尔失败的场景：

```cpp
// common/utils/redis_mutex.hpp
// 基于 "SET key value NX PX ttl_ms" 的短时互斥锁
// 非重入、非公平、无自动续约

class RedisMutex {
public:
    // ttl_ms: 锁自动过期时间（防止持锁方 crash 后死锁）
    RedisMutex(std::shared_ptr<sw::redis::RedisCluster> redis,
               const std::string &key, int ttl_ms = 5000);

    // 尝试获取锁，阻塞直到成功或超时
    bool try_lock(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

    // 释放锁（Lua CAS: 仅 owner 一致时 DEL）
    void unlock();

private:
    std::string _key;
    std::string _token;  // 随机 token，保证只有持锁方能释放
    int _ttl_ms;
};
```

**使用场景**：

| 场景 | key 模式 | ttl |
|------|----------|-----|
| Members warm 互斥（分布式 singleflight） | `im:lock:warm:members:{ssid}` | 5s |
| UserInfo warm 互斥 | `im:lock:warm:user:{uid}` | 3s |
| SeqGen backfill 互斥（多 Message 实例启动竞争） | `im:lock:backfill:seq` | 30s |

### 3.4 为什么不用 Redlock

Redlock 要求 N 个**独立** Redis 实例（不需要是 Cluster 节点），而 ChatNow 的 Redis Cluster 节点之间是 gossip 互联的，不满足 Redlock 的"独立实例"前提。在容器/虚拟化环境中，Redis 进程的时钟跳跃风险也无法消除。

---

## 4. 缓存防护体系

### 4.1 防击穿（Cache Stampede）

**已有设计**：`InflightRegistry`（per-key 进程内互斥 + double-check），见 `2026-05-13-cache-strategy-redesign.md` §3。

**新增增强**：与 L1 本地缓存联动。

```
resolve_members(ssid):
  ① L1 hit → return（快速路径，~ns）
  ② L1 miss → InflightRegistry.acquire(ssid)
  ③ double-check L1（可能其他线程刚 warm 完）
  ④ double-check L2 Redis
  ⑤ 穿透 RPC → warm L2 → warm L1 → release
```

⚠️ **注意**：InflightRegistry 是进程内的，多实例间仍有并发穿透可能。高频场景（如大群热点）建议叠加 RedisMutex（Tier 2 锁）做跨实例互斥。

### 4.2 防穿透（Cache Penetration）

对**确认不存在**的数据缓存空标记：

```cpp
// MembersResolver::resolve(ssid) 末尾:
if (members.empty()) {
    // 会话不存在或已解散：缓存空标记 60s，避免反复穿透
    _members_cache.warm_sentinel(ssid, std::chrono::seconds(60));
    _local_cache.set("members:" + ssid, {}, std::chrono::seconds(60));
    return {};
}
```

```cpp
// Members 类新增:
void warm_sentinel(const std::string &ssid, std::chrono::seconds ttl) {
    try {
        std::string k = key::kMembers + ssid;
        _c->sadd(k, "__sentinel__");  // 特殊标记值
        _c->expire(k, ttl);
    } catch (...) { ... }
}
```

读路径判别：
```cpp
auto members = _members_cache.list(ssid);
if (members.size() == 1 && members[0] == "__sentinel__") {
    return {};  // 确认不存在的会话
}
```

### 4.3 防雪崩（Cache Avalanche）

**已有设计**：`randomized_ttl()`，见 `2026-05-13-cache-strategy-redesign.md` §2。

本设计扩展应用范围：**所有** TTL 操作都走随机偏移（不仅是 Members），包括：

| 缓存 | base TTL | 随机偏移范围 |
|------|----------|-------------|
| Members | 30min | 24-36min |
| UserInfo | 1h | 48-72min |
| OnlineRoute | 120s | 96-144s |
| L1 OnlineRoute | 2s | 1.6-2.4s |
| L1 Members | 8s | 6.4-9.6s |
| L1 UserInfo | 45s | 36-54s |

---

## 5. 服务层改动

### 5.1 Transmite

```
TransmiteServerBuilder 新增:
  - _redis_cluster    → 替代 _redis
  - _local_cache_members → LocalCache<vector<string>>
  - _local_cache_user    → LocalCache<UserInfo>

TransmiteServiceImpl 新增:
  - resolve_members_with_cache()   → L1 → L2 → Inflight → RPC
  - resolve_user_with_cache()      → L1 → L2 → UserInfoCache → RPC
```

### 5.2 Push

```
PushServerBuilder 新增:
  - _redis_cluster         → 替代 _redis
  - _local_cache_route     → LocalCache<RouteEntry>

PushServiceImpl 新增:
  - online_route_with_cache()  → L1 → L2 → bind/touch
  - _on_close → 清理本地路由缓存
```

### 5.3 Message

```
MessageServerBuilder 新增:
  - _redis_cluster → 替代 _redis
  - _leader_election_reaper → 替代 PushOutbox::try_acquire_reaper_lease (Redis Lua)

消息量小的服务无需 L1 本地缓存（Message 是 DB/ES 消费端，缓存读写频率低）
```

### 5.4 ChatSession

```
ChatSessionServerBuilder 新增:
  - _redis_cluster → 替代 _redis

不需要 L1 本地缓存（ChatSession 是缓存写入方，不是高频读取方）
```

---

## 6. 在线路由可靠性增强

### 6.1 OnlineRoute TTL 优化

| 参数 | 当前 | 改后 | 理由 |
|------|------|------|------|
| OnlineRoute TTL | 120s | **30s** | 减少实例 crash 后的路由残留窗口 |
| 心跳续期间隔 | 当前心跳周期 | **≤15s** | 保证 TTL 在心跳周期内至少续约一次 |
| L1 本地路由缓存 TTL | 无 | **1-3s** | 削减 Redis 读，不影响一致性 |

### 6.2 Push 实例关停清理

在 `PushServer::stop()` 中增加：

```cpp
void PushServer::stop() {
    _ws_server->stop();

    // 遍历所有连接，清理 OnlineRoute
    for (const auto &[uid, conn] : _connections) {
        for (const auto &device_id : conn.devices()) {
            _online_route->unbind(uid, device_id, _instance_id);
        }
        // 同时清理本地和 L1 缓存
        _local_cache_route->invalidate("route:" + uid);
    }

    // 释放 etcd reaper 租约
    _leader_election->stop();
}
```

### 6.3 僵死实例扫描（新增）

独立后台线程在 Push 服务中运行，周期性扫描 OnlineRoute 中指向不可达实例的路由：

```cpp
void PushServer::reap_stale_routes_() {
    // 每 30s 执行
    // 1. 从 etcd 获取当前在线的 Push 实例列表
    // 2. SCAN im:online:* 所有 key
    // 3. 对于每个 (uid, device_id) → instance_id 映射
    //    如果 instance_id 不在在线列表中 → HDEL
    // 4. 可选：转为 LeaderElection 保护的单 reaper 执行
}
```

---

## 7. 监控指标

### 7.1 Redis Cluster 指标

| 指标 | 用途 |
|------|------|
| `redis_commands_total{cmd, node}` | 每节点命令分布 |
| `redis_command_duration_ms{cmd, node}` | P50/P99 延迟 |
| `redis_pool_active_connections{node}` | 连接池饱和度 |
| `redis_cluster_slots_migrating` | slot 迁移状态 |
| `redis_memory_used_bytes{node}` | 内存水位 |

### 7.2 本地缓存指标

| 指标 | 用途 |
|------|------|
| `local_cache_hit_ratio{cache_name}` | 命中率 (target > 90%) |
| `local_cache_size{cache_name}` | 条目数 |
| `local_cache_evictions_total{cache_name}` | 淘汰次数 |

### 7.3 业务级告警

| 告警 | 条件 | 级别 |
|------|------|------|
| Redis Cluster 节点 down | `redis_up{node} == 0` > 1min | CRITICAL |
| 本地缓存命中率骤降 | `rate(local_cache_miss[5m])` 突增 3x | WARNING |
| Reaper 选举频繁切换 | `leader_election_changes_total` > 2 / 10min | WARNING |
| Redis 连接池耗尽 | `redis_pool_active >= pool_size` | CRITICAL |

---

## 8. 改动清单

| 文件 | 改动 | 行数（估） |
|------|------|-----------|
| `common/dao/data_redis.hpp` | 新增 `RedisClusterFactory`；`Members` 新增 `warm_sentinel`、`touch_ttl` | +50 |
| `common/utils/local_cache.hpp` | **新增** 通用 L1 本地缓存模板 | +80 |
| `common/utils/redis_mutex.hpp` | **新增** Redis 短时互斥锁 | +55 |
| `common/infra/leader_election.hpp` | **新增** etcd 选举锁封装 | +80 |
| `transmite/source/transmite_server.h` | 集成 L1 Members/UserInfo 缓存 + RedisCluster | +50 |
| `push/source/push_server.h` | 集成 L1 OnlineRoute 缓存 + RedisCluster + 关停清理 + stale reaper | +70 |
| `message/source/message_server.h` | RedisCluster + etcd reaper 选举替换 Lua CAS | +40 |
| `message/source/message_server.h` | `backfill_seq_from_db_` 增加 RedisMutex 跨实例互斥 | +15 |
| `chatsession/source/chatsession_server.h` | RedisCluster + Members 增量写（add/remove） | +25 |
| `identity/source/identity_server.h` | RedisCluster | +10 |
| `gateway/source/gateway_server.h` | RedisCluster | +8 |
| `media/source/media_server.h` | RedisCluster | +8 |
| `conf/*.conf` (8个) | `redis_host/port` → `redis_seeds` | +16 |
| `docker-compose.yml` | Redis Cluster 6 节点容器编排 | +30 |
| **总计** | | **~537 行** |

---

## 9. 上线顺序

按依赖关系分阶段：

### 阶段 1：基础设施（无业务变更，独立验证）

1. Redis Cluster 部署（docker-compose 编排 6 节点）
2. `RedisClusterFactory` 实现
3. 所有服务配置切换到 `redis_seeds`
4. **验证**：Redis Cluster 健康检查、failover 演练、slot 分布均匀

### 阶段 2：分布式锁统一（依赖阶段 1）

5. `LeaderElection` 封装
6. PushOutbox / ESOutbox / CrossInstanceOutbox reaper 从 Lua CAS 迁移到 etcd 选举
7. Snowflake worker_id 分配统一走 etcd LeaderElection
8. `RedisMutex` 实现（短时互斥）
9. SeqGen backfill 增加 RedisMutex 跨实例互斥
10. **验证**：reaper 无脑裂、worker_id 无撞号

### 阶段 3：L1 本地缓存（依赖阶段 1）

11. `LocalCache<T>` 实现
12. Push 接入 OnlineRoute L1 缓存
13. Transmite 接入 Members / UserInfo L1 缓存
14. 关停清理 + stale route reaper
15. OnlineRoute TTL 调优（120s → 30s）
16. **验证**：命中率 > 90%、TTL 过期正确、关停清理完整

### 阶段 4：防护体系（依赖阶段 3）

17. 空值缓存 sentinel（防穿透）
18. 全量 TTL 随机偏移（防雪崩）
19. **验证**：穿透场景不击穿后端、全服务重启后 TTL 分布均匀

### 阶段 5：监控接入

20. Redis Cluster exporter + Prometheus
21. 本地缓存 hit/miss/eviction bvar
22. 告警规则配置

---

## 10. 测试要点

### Redis Cluster
- [ ] 单节点宕机：自动 failover，无数据丢失，无消息丢失
- [ ] 全集群重启：SeqGen backfill 正确回填，无 seq 撞号
- [ ] 多实例并发 INCR 同一 key：seq 严格单调递增
- [ ] Pipeline INCR 跨 slot：sw::redis++ 自动拆分转发

### 多级缓存
- [ ] L1 hit → 不访问 Redis（验证通过日志/计数器）
- [ ] L1 miss → L2 hit → 回填 L1 → 下次 L1 hit
- [ ] L1 TTL 过期 → L2 读到新数据 → L1 更新
- [ ] ChatSession 加人后 10s 内 L1 自然过期，新成员出现在 members 列表中

### 分布式锁
- [ ] etcd reaper 选举：主 crash 后 backup 在 lease_ttl 内接管
- [ ] 无脑裂：一个 etcd lease 同时只有一个 holder
- [ ] RedisMutex crash 安全：持锁方 crash 后 TTL 过期自动释放

### 在线路由
- [ ] Push 实例 crash → 30s 内路由清理（TTL 过期）
- [ ] Push 优雅关停 → 主动 unbind + L1 清理，< 1s 生效
- [ ] 用户切换 Push 实例 → 新路由即时生效，旧路由 TTL 自清

---

## 11. 总结

本设计与 `2026-05-13-cache-strategy-redesign.md`（数据层改进）形成互补：

| 层面 | 2026-05-13 spec（数据层） | 本 spec（基础设施层） |
|------|--------------------------|---------------------|
| Redis 拓扑 | 单实例（未涉及） | Redis Cluster 3 主 3 从 |
| 缓存层级 | 仅 Redis | L1 本地 + L2 Redis Cluster |
| 锁机制 | Lua CAS | etcd 选举 + Redis 短时互斥分层 |
| 防护 | 随机 TTL + InflightRegistry | + 空值 sentinel + 全量随机 TTL |
| 在线路由 | TTL 120s | TTL 30s + 关停清理 + stale reaper |

两个 spec 共同覆盖了 review 报告中识别的所有硬伤和设计缺陷。
