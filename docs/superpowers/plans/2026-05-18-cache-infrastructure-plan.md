# Cache Infrastructure Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade from single-instance Redis to Redis Cluster with L1 local cache, layered distributed locks (etcd + Redis), and cache protection (penetration/stampede/avalanche).

**Architecture:** Redis Cluster 3-master-3-slave with natural CRC16 sharding. `RedisClient` adapter delegates to single `Redis` or `RedisCluster` transparently. L1 in-process `LocalCache<T>` for OnlineRoute/Members/UserInfo with InflightRegistry double-check for stampede protection. etcd Transaction CAS for reaper/snowflake leader elections; `RedisMutex` for sub-second cache-warm mutex. Sentinel null-cache for penetration, randomized TTL for avalanche.

**Tech Stack:** C++17, sw::redis++ (Redis & RedisCluster), etcd-cpp-apiv3, brpc, websocketpp, gflags

**Spec:** `docs/superpowers/specs/2026-05-18-cache-infrastructure-redesign.md`

---

## File Structure

```
common/dao/data_redis.hpp           — +RedisClient adapter, +RedisClusterFactory, +Members::warm_sentinel, touch_ttl
common/utils/local_cache.hpp        — NEW: LocalCache<T> template
common/utils/inflight.hpp           — NEW: InflightRegistry (per-key process-internal mutex)
common/utils/redis_mutex.hpp        — NEW: RedisMutex (SET NX PX + Lua CAS unlock)
common/infra/leader_election.hpp    — NEW: etcd LeaderElection (Transaction CAS)
common/utils/random_ttl.hpp         — NEW: randomized_ttl() utility

push/source/push_server.h           — L1 OnlineRoute cache, shutdown cleanup, stale route reaper + LeaderElection
push/source/push_server.cc          — etcd client wireup, redis_seeds flag
transmite/source/transmite_server.h — L1 Members/UserInfo cache with InflightRegistry + RedisMutex
transmite/source/transmite_server.cc— redis_seeds flag
message/source/message_server.h     — NEW: PushOutbox/ESOutbox reaper threads + LeaderElection + RedisMutex for backfill
message/source/message_server.cc    — etcd client wireup, redis_seeds flag, reaper start/stop wiring
identity/source/identity_server.*   — redis_seeds flag
gateway/source/gateway_server.*     — redis_seeds flag
media/source/media_server.*         — redis_seeds flag
conversation/source/conversation_server.* — redis_seeds flag
presence/source/presence_server.*   — redis_seeds flag
common/infra/snowflake.hpp          — etcd LeaderElection for worker_id allocation
conf/*.conf (8 files)               — redis_seeds field
docker-compose.yml                  — replace single redis with 6-node Cluster
```

---

## Phase 1: Redis Cluster Infrastructure

### Task 1: Create RedisClient type-erased adapter + RedisClusterFactory

**Files:**
- Modify: `common/dao/data_redis.hpp`

**Why:** Existing 15 cache classes all store `std::shared_ptr<sw::redis::Redis>`. Rather than duplicating every class for `RedisCluster`, create a `RedisClient` adapter that delegates to either backend via a simple pointer check. This is a lightweight wrapper — zero heap allocation on the hot path, just a branch per Redis call.

> **设计偏离**: Spec §1.3 设计各服务直接持有 `std::shared_ptr<sw::redis::RedisCluster>`，通过 `RedisClusterFactory` 创建。Plan 引入 `RedisClient` 类型擦除适配器作为中间层，目的是避免修改 15 个 cache 类的方法签名（它们仍调用 `_c->get()` 等，无需感知后端是单机还是 Cluster）。这是有意的工程权衡——牺牲一次分支判断（~1ns）换取最小化代码改动范围。如果后续移除单机模式支持，可以删除适配器直接使用 `RedisCluster`。

- [ ] **Step 1: Add RedisClient adapter class**

Open `common/dao/data_redis.hpp`. Add `#include <sw/redis++/redis_cluster.h>` after line 20 (the existing `#include <sw/redis++/redis++.h>`). Then add the following class between the doc comment block and `namespace chatnow {` (or right after the opening namespace brace):

```cpp
// Type-erased Redis client: delegates to sw::redis::Redis (single) or
// sw::redis::RedisCluster. All cache classes use RedisClient::ptr instead
// of std::shared_ptr<sw::redis::Redis> directly. The branch per call is
// negligible (~1 ns) compared to Redis network latency (~0.5 ms).
class RedisClient {
public:
    using ptr = std::shared_ptr<RedisClient>;

    RedisClient(std::shared_ptr<sw::redis::Redis> r) : _r(std::move(r)) {}
    RedisClient(std::shared_ptr<sw::redis::RedisCluster> rc) : _rc(std::move(rc)) {}

    // --- String commands ---
    sw::redis::OptionalString get(const std::string &key) {
        return _rc ? _rc->get(key) : _r->get(key);
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::seconds ttl = std::chrono::seconds(0)) {
        return _rc ? _rc->set(key, val, ttl) : _r->set(key, val, ttl);
    }
    bool set(const std::string &key, const std::string &val,
             std::chrono::milliseconds ttl) {
        return _rc ? _rc->set(key, val, ttl) : _r->set(key, val, ttl);
    }
    long long del(const std::string &key) {
        return _rc ? _rc->del(key) : _r->del(key);
    }
    void expire(const std::string &key, std::chrono::seconds ttl) {
        _rc ? _rc->expire(key, ttl) : _r->expire(key, ttl);
    }
    long long incr(const std::string &key) {
        return _rc ? _rc->incr(key) : _r->incr(key);
    }

    // --- Set commands ---
    template <typename T>
    long long sadd(const std::string &key, const T &member) {
        return _rc ? _rc->sadd(key, member) : _r->sadd(key, member);
    }
    template <typename It>
    long long sadd(const std::string &key, It first, It last) {
        return _rc ? _rc->sadd(key, first, last) : _r->sadd(key, first, last);
    }
    template <typename Out>
    void smembers(const std::string &key, Out out) {
        _rc ? _rc->smembers(key, out) : _r->smembers(key, out);
    }
    template <typename T>
    long long srem(const std::string &key, const T &member) {
        return _rc ? _rc->srem(key, member) : _r->srem(key, member);
    }
    long long scard(const std::string &key) {
        return _rc ? _rc->scard(key) : _r->scard(key);
    }

    // --- Hash commands ---
    long long hset(const std::string &key, const std::string &field, const std::string &val) {
        return _rc ? _rc->hset(key, field, val) : _r->hset(key, field, val);
    }
    sw::redis::OptionalString hget(const std::string &key, const std::string &field) {
        return _rc ? _rc->hget(key, field) : _r->hget(key, field);
    }
    long long hdel(const std::string &key, const std::string &field) {
        return _rc ? _rc->hdel(key, field) : _r->hdel(key, field);
    }
    template <typename Out>
    void hkeys(const std::string &key, Out out) {
        _rc ? _rc->hkeys(key, out) : _r->hkeys(key, out);
    }
    template <typename Out>
    void hgetall(const std::string &key, Out out) {
        _rc ? _rc->hgetall(key, out) : _r->hgetall(key, out);
    }
    long long hlen(const std::string &key) {
        return _rc ? _rc->hlen(key) : _r->hlen(key);
    }

    // --- Sorted Set commands ---
    long long zadd(const std::string &key, const std::string &member, double score) {
        return _rc ? _rc->zadd(key, member, score) : _r->zadd(key, member, score);
    }
    long long zadd(const std::string &key, const std::string &member, double score,
                   sw::redis::UpdateType type) {
        return _rc ? _rc->zadd(key, member, score, type) : _r->zadd(key, member, score, type);
    }
    long long zrem(const std::string &key, const std::string &member) {
        return _rc ? _rc->zrem(key, member) : _r->zrem(key, member);
    }
    template <typename Out>
    void zrange(const std::string &key, long long start, long long stop, Out out) {
        _rc ? _rc->zrange(key, start, stop, out) : _r->zrange(key, start, stop, out);
    }
    template <typename Out>
    void zrangebyscore(const std::string &key,
                       const sw::redis::BoundedInterval<double> &interval,
                       const sw::redis::LimitOptions &opts, Out out) {
        _rc ? _rc->zrangebyscore(key, interval, opts, out)
            : _r->zrangebyscore(key, interval, opts, out);
    }

    // --- Lua scripting ---
    template <typename Ret, typename KeyIt, typename ArgIt>
    Ret eval(const std::string &script, KeyIt key_first, KeyIt key_last,
             ArgIt arg_first, ArgIt arg_last) {
        return _rc ? _rc->eval<Ret>(script, key_first, key_last, arg_first, arg_last)
                   : _r->eval<Ret>(script, key_first, key_last, arg_first, arg_last);
    }
    template <typename Ret, typename KeyIt, typename ArgIt, typename Out>
    Ret eval(const std::string &script, KeyIt key_first, KeyIt key_last,
             ArgIt arg_first, ArgIt arg_last, Out out) {
        return _rc ? _rc->eval<Ret>(script, key_first, key_last, arg_first, arg_last, out)
                   : _r->eval<Ret>(script, key_first, key_last, arg_first, arg_last, out);
    }

    // --- Pipeline (used by SeqGen::next_user_seq_batch) ---
    auto pipeline() {
        return _rc ? _rc->pipeline() : _r->pipeline();
    }

    // --- SCAN (used by PresenceRedis::get_devices) ---
    template <typename Out>
    long long scan(long long cursor, const std::string &pattern, long long count, Out out) {
        return _rc ? _rc->scan(cursor, pattern, count, out)
                   : _r->scan(cursor, pattern, count, out);
    }

    bool is_cluster() const { return _rc != nullptr; }

private:
    std::shared_ptr<sw::redis::Redis> _r;
    std::shared_ptr<sw::redis::RedisCluster> _rc;
};
```

- [ ] **Step 2: Update all 15 cache classes' constructors and members**

In every class in `data_redis.hpp` (`Session`, `Status`, `Codes`, `SeqGen`, `LastMessage`, `DeviceSet`, `ReadAck`, `Members`, `OnlineRoute`, `RateLimiter`, `PushOutbox`, `CrossInstanceOutbox`, `ESOutbox`, `UnackedPush`, `PresenceRedis`):

Replace `std::shared_ptr<sw::redis::Redis>` with `RedisClient::ptr` in both the constructor parameter type and the private `_c` member type. The method implementations don't change — they still call `_c->get()`, `_c->set()`, etc.

Example for `Session`:

```cpp
// Before:
class Session {
public:
    using ptr = std::shared_ptr<Session>;
    Session(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}
    // ...
private:
    std::shared_ptr<sw::redis::Redis> _c;
};

// After:
class Session {
public:
    using ptr = std::shared_ptr<Session>;
    Session(const RedisClient::ptr &c) : _c(c) {}
    // ... methods unchanged
private:
    RedisClient::ptr _c;
};
```

Apply the same pattern to all 15 classes (lines 101, 130, 156, 197, 277, 305, 341, 390, 437, 497, 533, 605, 686, 754, 857).

- [ ] **Step 3: Add RedisClusterFactory**

After the existing `RedisClientFactory` class (after line 95), add:

```cpp
/* brief: Redis Cluster 工厂 — 通过种子节点自动发现集群拓扑 */
class RedisClusterFactory
{
public:
    static std::shared_ptr<sw::redis::RedisCluster> create(
        const std::string &seed_nodes_csv,  // "host1:6379,host2:6379,host3:6379"
        int pool_size = 16,
        bool keep_alive = true)
    {
        // 解析所有种子节点
        std::vector<std::pair<std::string, uint16_t>> seeds;
        {
            std::istringstream ss(seed_nodes_csv);  // #include <sstream>
            std::string token;
            while (std::getline(ss, token, ',')) {
                auto colon = token.find(':');
                if (colon == std::string::npos) continue;
                seeds.emplace_back(
                    token.substr(0, colon),
                    static_cast<uint16_t>(std::stoi(token.substr(colon + 1))));
            }
        }
        if (seeds.empty()) {
            throw std::runtime_error("RedisClusterFactory: 无有效种子节点");
        }

        sw::redis::ConnectionPoolOptions popts;
        popts.size = pool_size;
        popts.wait_timeout = std::chrono::milliseconds(500);
        popts.connection_lifetime = std::chrono::minutes(30);

        // 逐个尝试种子节点，直到成功连接（sw::redis++ RedisCluster 仅需一个种子
        // 即可通过 CLUSTER SLOTS 自动发现完整拓扑）
        std::string last_error;
        for (const auto &[host, port] : seeds) {
            try {
                sw::redis::ConnectionOptions copts;
                copts.host = host;
                copts.port = port;
                copts.keep_alive = keep_alive;
                copts.connect_timeout = std::chrono::milliseconds(2000);
                copts.socket_timeout  = std::chrono::milliseconds(2000);

                auto cluster = std::make_shared<sw::redis::RedisCluster>(copts, popts);
                // 验证连接可用（立即尝试一个轻量命令）
                cluster->ping("cluster-seed-check");
                LOG_INFO("RedisClusterFactory: 通过种子 {}:{} 成功连接集群", host, port);
                return cluster;
            } catch (std::exception &e) {
                last_error = e.what();
                LOG_WARN("RedisClusterFactory: 种子 {}:{} 连接失败 ({}), 尝试下一个",
                         host, port, last_error);
            }
        }
        throw std::runtime_error("RedisClusterFactory: 所有种子节点连接失败 — " + last_error);
    }
};
```

- [ ] **Step 4: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -30
```

Expected: All targets compile without errors. The `sw::redis++/redis_cluster.h` header is part of the sw::redis++ library (version 1.3.0+).

- [ ] **Step 5: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "feat: add RedisClient adapter + RedisClusterFactory, switch all 15 cache classes to RedisClient::ptr"
```

---

### Task 2: Add `--redis_seeds` flag + dual-mode init in all service builders

**Files:**
- Modify: `transmite/source/transmite_server.cc` + `.h`
- Modify: `message/source/message_server.cc` + `.h`
- Modify: `push/source/push_server.cc` + `.h`
- Modify: `identity/source/identity_server.cc` + `.h`
- Modify: `gateway/source/gateway_server.cc` + `.h`
- Modify: `media/source/media_main.cc` + `media/source/media_server.h`
- Modify: `conversation/source/conversation_server.cc` + `.h`
- Modify: `presence/source/presence_server.cc` + `.h`

> **Note**: Spec §8 列出了 `chatsession_server.h`（即 Conversation 服务），但仅需 +25 行做 Members 写路径（add/remove）集成。Plan 将 Conversation 服务包含在 dual-mode init 中。实施前需确认：(a) 当前代码库中 Conversation 服务与 Spec 中的 ChatSession 是否为同一服务；(b) Members 写路径是否需要额外改动（如果当前 Members 类已基于 RedisClient 工作，则仅需 dual-mode init 即可）。

- [ ] **Step 1: Add `--redis_seeds` flag to all 8 main.cc files**

In each `*_server.cc` / `*_main.cc`, add after the existing `DEFINE_string(redis_host, ...)` line:

```cpp
DEFINE_string(redis_seeds, "", "Redis Cluster 种子节点（逗号分隔，如 host1:6379,host2:6379）");
```

Files to edit (find the `DEFINE_string(redis_host` line, insert after):
- `transmite/source/transmite_server.cc`
- `message/source/message_server.cc`
- `push/source/push_server.cc`
- `identity/source/identity_server.cc`
- `gateway/source/gateway_server.cc`
- `media/source/media_main.cc`
- `conversation/source/conversation_server.cc`
- `presence/source/presence_server.cc`

- [ ] **Step 2: Update builder classes to support both Redis single and Cluster**

In each builder header (`*_server.h`), add `set_redis_seeds()` method and modify `make_redis_object()` to check for `--redis_seeds`:

```cpp
void set_redis_seeds(const std::string &seeds) { _redis_seeds = seeds; }

// Change redis member from std::shared_ptr<sw::redis::Redis> to:
std::string _redis_seeds;
RedisClient::ptr _redis_client;

// Update make_redis_object to use RedisClient wrapper:
void make_redis_object(const std::string &host, uint16_t port, int db,
                       bool keep_alive, int pool_size)
{
    if (!_redis_seeds.empty()) {
        auto cluster = RedisClusterFactory::create(_redis_seeds, pool_size, keep_alive);
        _redis_client = std::make_shared<RedisClient>(cluster);
    } else {
        auto redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _redis_client = std::make_shared<RedisClient>(redis);
    }
    // ... construct cache objects with _redis_client as before
}
```

Update `make_redis_object()` in each builder:
- **Transmite** (line ~493): constructs `SeqGen`, `Members`, `RateLimiter`
- **Push** (line ~687): constructs `OnlineRoute`, `UnackedPush`, `CrossInstanceOutbox`
- **Message** (line ~894): constructs `SeqGen`, `PushOutbox`, `ESOutbox`
- **Identity**: constructs `Session`, `Codes`, `DeviceSet`
- **Gateway**: constructs `Session`
- **Media**: constructs `RateLimiter`
- **Conversation**: constructs `Members`
- **Presence**: constructs `PresenceRedis`

In each service's main.cc, add the `set_redis_seeds()` call before `make_redis_object()`:

```cpp
tsb.set_redis_seeds(FLAGS_redis_seeds);
tsb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                       FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
```

- [ ] **Step 3: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -30
```

Expected: All 8 service binaries compile.

- [ ] **Step 4: Commit**

```bash
git add transmite/source/transmite_server.cc transmite/source/transmite_server.h \
        message/source/message_server.cc message/source/message_server.h \
        push/source/push_server.cc push/source/push_server.h \
        identity/source/identity_server.cc identity/source/identity_server.h \
        gateway/source/gateway_server.cc gateway/source/gateway_server.h \
        media/source/media_main.cc media/source/media_server.h \
        conversation/source/conversation_server.cc conversation/source/conversation_server.h \
        presence/source/presence_server.cc presence/source/presence_server.h
git commit -m "feat: dual-mode Redis init (single + Cluster) in all 8 service builders"
```

---

### Task 3: Update all 8 config files with `-redis_seeds`

**Files:**
- Modify: `conf/transmite_server.conf`
- Modify: `conf/message_server.conf`
- Modify: `conf/push_server.conf`
- Modify: `conf/identity_server.conf`
- Modify: `conf/gateway_server.conf`
- Modify: `conf/media_server.conf`
- Modify: `conf/conversation_server.conf`
- Modify: `conf/presence_server.conf`

- [ ] **Step 1: Add `-redis_seeds=` line**

In each `.conf` file, add after the `-redis_db=X` line:

```
-redis_seeds=
```

When empty → single-instance mode (`-redis_host`/`-redis_port`). When populated → Cluster mode.

- [ ] **Step 2: Commit**

```bash
git add conf/transmite_server.conf conf/message_server.conf conf/push_server.conf \
        conf/identity_server.conf conf/gateway_server.conf conf/media_server.conf \
        conf/conversation_server.conf conf/presence_server.conf
git commit -m "feat: add -redis_seeds config field to all 8 service configs"
```

---

### Task 4: 6-node Redis Cluster in docker-compose

**Files:**
- Modify: `docker-compose.yml`

- [ ] **Step 1: Replace single `redis:` service with 6 Cluster nodes + init sidecar**

Replace the existing `redis:` block with:

```yaml
  redis-node1:
    image: redis:7.2.5
    container_name: redis-node1
    command: redis-server --port 6379 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-1.aof
    volumes:
      - ./middle/data/redis/node1:/data:rw
    ports:
      - "6379:6379"
    restart: always

  redis-node2:
    image: redis:7.2.5
    container_name: redis-node2
    command: redis-server --port 6380 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-2.aof
    volumes:
      - ./middle/data/redis/node2:/data:rw
    ports:
      - "6380:6380"
    restart: always

  redis-node3:
    image: redis:7.2.5
    container_name: redis-node3
    command: redis-server --port 6381 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-3.aof
    volumes:
      - ./middle/data/redis/node3:/data:rw
    ports:
      - "6381:6381"
    restart: always

  redis-node4:
    image: redis:7.2.5
    container_name: redis-node4
    command: redis-server --port 6382 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-4.aof
    volumes:
      - ./middle/data/redis/node4:/data:rw
    ports:
      - "6382:6382"
    restart: always

  redis-node5:
    image: redis:7.2.5
    container_name: redis-node5
    command: redis-server --port 6383 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-5.aof
    volumes:
      - ./middle/data/redis/node5:/data:rw
    ports:
      - "6383:6383"
    restart: always

  redis-node6:
    image: redis:7.2.5
    container_name: redis-node6
    command: redis-server --port 6384 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-6.aof
    volumes:
      - ./middle/data/redis/node6:/data:rw
    ports:
      - "6384:6384"
    restart: always

  redis-cluster-init:
    image: redis:7.2.5
    container_name: redis-cluster-init
    depends_on:
      - redis-node1
      - redis-node2
      - redis-node3
      - redis-node4
      - redis-node5
      - redis-node6
    entrypoint: |
      /bin/sh -c "
      echo 'Waiting for all Redis nodes...' &&
      sleep 10 &&
      echo 'Creating 3-master 3-slave cluster...' &&
      echo yes | redis-cli --cluster create \
        redis-node1:6379 redis-node2:6380 redis-node3:6381 \
        redis-node4:6382 redis-node5:6383 redis-node6:6384 \
        --cluster-replicas 1 &&
      echo 'Verifying cluster...' &&
      redis-cli --cluster check redis-node1:6379 &&
      echo 'Cluster ready.' &&
      tail -f /dev/null
      "
    restart: "no"
```

- [ ] **Step 2: Update service depends_on and entrypoint commands**

For all services that use Redis, change `depends_on: - redis` to `depends_on: - redis-cluster-init`. Add `redis-cluster-init` to all other service depends_on lists.

Add `-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384` to each service's entrypoint command (全部 6 个种子节点，提升启动容错)。

- [ ] **Step 3: Commit**

```bash
git add docker-compose.yml
git commit -m "feat: replace single Redis with 6-node Cluster (3M3S) in docker-compose"
```

---

## Phase 2: Distributed Locks

### Task 5: Create LeaderElection component with etcd Transaction CAS

**Files:**
- Create: `common/infra/leader_election.hpp`

- [ ] **Step 1: Write LeaderElection class using full Transaction CAS**

```cpp
// common/infra/leader_election.hpp
#pragma once

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Transaction.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include "infra/logger.hpp"

namespace chatnow {

// etcd Lease + Transaction CAS 选举锁。
// 使用 etcdv3 Transaction 保证"仅当 key 不存在时才写入"，消除双主窗口。
class LeaderElection {
public:
    using ptr = std::shared_ptr<LeaderElection>;

    // election_key:  e.g. "/chatnow/reaper/push_outbox/leader"
    // instance_id:   唯一实例标识
    // lease_ttl_sec: 租约 TTL（超时后 key 自动删除，其他实例可竞选）
    // on_acquired / on_lost: 回调（在独立竞选线程中调用，需自行处理线程安全）
    LeaderElection(std::shared_ptr<etcd::Client> etcd,
                   const std::string &election_key,
                   const std::string &instance_id,
                   int lease_ttl_sec,
                   std::function<void()> on_acquired,
                   std::function<void()> on_lost)
        : _etcd(std::move(etcd)), _key(election_key), _id(instance_id),
          _ttl(lease_ttl_sec), _on_acquired(std::move(on_acquired)),
          _on_lost(std::move(on_lost)) {}

    ~LeaderElection() { stop(); }

    void start() {
        _running = true;
        _thread = std::thread([this]() { campaign_loop_(); });
    }

    void stop() {
        _running = false;
        if (_keep_alive) {
            try { _keep_alive->Cancel(); } catch (...) {}
        }
        if (_thread.joinable()) _thread.join();
    }

    bool is_leader() const { return _is_leader.load(); }

private:
    void campaign_loop_() {
        while (_running) {
            try {
                // 1. 创建租约
                auto lease_resp = _etcd->leasegrant(_ttl).get();
                if (!lease_resp.is_ok()) {
                    LOG_WARN("LeaderElection leasegrant 失败: {}", lease_resp.error_message());
                    std::this_thread::sleep_for(std::chrono::seconds(_ttl / 2));
                    continue;
                }
                int64_t lease_id = lease_resp.value().lease();

                // 2. Transaction CAS:
                //    CMP: version(key) == 0  (key 不存在)
                //    THEN: put(key, our_id, lease_id)
                //    ELSE: get(key)  (看谁持有)
                etcd::Transaction txn;
                txn.setup_compare_version(_key, etcd::CompareResult::EQUAL, 0);
                txn.setup_put_success(_key, _id, lease_id);
                txn.setup_get_failure(_key);
                auto txn_resp = _etcd->txn(txn).get();

                if (txn_resp.is_ok() && txn_resp.value().succeeded()) {
                    // Won the election — start keep-alive
                    _keep_alive = _etcd->keepalive(lease_id).get();
                    _is_leader = true;
                    if (_on_acquired) _on_acquired();

                    // Hold until lease lost or stopped
                    _hold_leadership_(lease_id);

                    // Lost leadership
                    if (_is_leader.exchange(false)) {
                        try { _keep_alive->Cancel(); } catch (...) {}
                        if (_on_lost) _on_lost();
                    }
                } else {
                    // Someone else holds the key — back off and retry
                    LOG_DEBUG("LeaderElection: {} 已被占用，等待重试", _key);
                    try { _etcd->leaserevoke(lease_id).wait(); } catch (...) {}
                }
            } catch (std::exception &e) {
                LOG_ERROR("LeaderElection campaign 异常: {}", e.what());
            }

            if (_running) {
                std::this_thread::sleep_for(std::chrono::seconds(_ttl / 3));
            }
        }
    }

    void _hold_leadership_(int64_t lease_id) {
        while (_running && _is_leader) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // Check lease TTL periodically
            auto ttl_resp = _etcd->timetolive(lease_id).get();
            if (!ttl_resp.is_ok() || ttl_resp.value().ttl() <= 0) {
                LOG_WARN("LeaderElection lease {} 过期，失去 leader", lease_id);
                break;
            }
        }
    }

    std::shared_ptr<etcd::Client> _etcd;
    std::string _key;
    std::string _id;
    int _ttl;
    std::function<void()> _on_acquired;
    std::function<void()> _on_lost;

    std::thread _thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _is_leader{false};
    std::shared_ptr<etcd::KeepAlive> _keep_alive;
};

} // namespace chatnow
```

Key design point: `setup_compare_version(_key, EQUAL, 0)` checks if the key has never been created. This is the standard etcd "create if not exists" pattern. Two concurrent instances cannot both succeed — etcd's Raft consensus linearizes the transactions.

> **前置条件**: etcd-cpp-apiv3 >= v0.14.0（Transaction API 自此版本引入）。构建前必须在 Linux 构建主机上验证：
> ```bash
> grep -rn "class Transaction" $(find / -path '*/etcd/*.hpp' 2>/dev/null | head -5)
> ```
> 如果 Transaction API 不存在，**禁止降级为两阶段 PUT+GET 方案**——该方案不提供原子性保证，会导致脑裂双主，直接违背 Spec §3.2 的一致性要求。应升级 etcd-cpp-apiv3 库至 v0.14.0+。

- [ ] **Step 2: Build verify — 含 etcd Transaction API 编译验证**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/infra/leader_election.hpp
git commit -m "feat: add LeaderElection with etcd Transaction CAS for reaper/snowflake"
```

---

### Task 6: Create RedisMutex (short-lived distributed mutex)

**Files:**
- Create: `common/utils/redis_mutex.hpp`

- [ ] **Step 1: Write RedisMutex**

```cpp
// common/utils/redis_mutex.hpp
#pragma once

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include "common/dao/data_redis.hpp"
#include "infra/logger.hpp"

namespace chatnow {

// 基于 "SET key token NX PX ttl_ms" + Lua CAS unlock 的短时互斥锁。
// 用于 cache warm 互斥、backfill 协调等毫秒-秒级场景。
class RedisMutex {
public:
    // key:    逻辑锁名（自动加 "im:lock:" 前缀）
    // ttl_ms: 锁自动过期时间（防止持锁方 crash 后死锁）
    RedisMutex(RedisClient::ptr redis, const std::string &key, int ttl_ms = 5000)
        : _redis(std::move(redis)), _key("im:lock:" + key), _ttl_ms(ttl_ms)
    {
        _token = generate_token_();
    }

    // 阻塞直到获取锁或超时。返回是否获取成功。
    bool try_lock(std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            bool ok = _redis->set(_key, _token, std::chrono::milliseconds(_ttl_ms));
            if (ok) { _locked = true; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // 释放锁（Lua CAS: 仅 token 一致的持有者才能 DEL）
    void unlock() {
        if (!_locked) return;
        static const char *kUnlockLua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    return redis.call('DEL', KEYS[1]) "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {_key};
            std::vector<std::string> args = {_token};
            _redis->eval<long long>(kUnlockLua, keys.begin(), keys.end(),
                                    args.begin(), args.end());
        } catch (std::exception &e) {
            LOG_WARN("RedisMutex.unlock 失败 {}: {}", _key, e.what());
        }
        _locked = false;
    }

private:
    static std::string generate_token_() {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<unsigned long long> dist;
        return std::to_string(dist(rng));
    }

    RedisClient::ptr _redis;
    std::string _key;
    std::string _token;
    int _ttl_ms;
    bool _locked = false;
};

} // namespace chatnow
```

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/utils/redis_mutex.hpp
git commit -m "feat: add RedisMutex for short-lived distributed locks"
```

---

### Task 7: Create and migrate Message reapers (PushOutbox + ESOutbox) + Push CrossInstanceOutbox to etcd LeaderElection

**Files:**
- Modify: `message/source/message_server.h`
- Modify: `message/source/message_server.cc`
- Modify: `push/source/push_server.h`
- Modify: `push/source/push_server.cc`

**IMPORTANT**: 当前代码中 PushOutbox 和 ESOutbox 的 reaper 线程**不存在**。`try_acquire_reaper_lease` / `release_reaper_lease` 方法只是定义在 outbox 类上，但从未被调用；`_reaper_owner` 字段也是死的。只有 Push 中的 `CrossInstanceOutbox` 有 `start_cross_outbox_reaper()` 线程。因此本 task 需要先**创建** reaper 线程，再接入 LeaderElection。

> **注意 (未验证假设)**: Step 0 的 reaper 线程代码中引用了 `_mq_client->publish()` 和 `_es_client->index()`。实施前需确认 `MessageServerBuilder` 中是否已有 `_mq_client` 和 `_es_client` 成员及其对应类型。

- [ ] **Step 0: Create PushOutbox and ESOutbox reaper threads in MessageServer**

在 `MessageServerBuilder` 中新增 reaper 线程启动方法（参照 `PushServer::start_cross_outbox_reaper()` 的模式）：

```cpp
// message/source/message_server.h — MessageServerBuilder 新增:

void start_push_outbox_reaper() {
    _push_reaper_running = true;
    _push_reaper_thread = std::thread([this]() {
        while (_push_reaper_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!_push_reaper_election || !_push_reaper_election->is_leader())
                continue;
            try {
                auto items = _push_outbox->dequeue(50);
                for (const auto &item : items) {
                    // 重新投递到 MQ / Push 通道
                    _mq_client->publish(item.topic, item.payload);
                    _push_outbox->ack(item.id);
                }
            } catch (std::exception &e) {
                LOG_WARN("PushOutbox reaper 异常: {}", e.what());
            }
        }
        _push_outbox->release_reaper_lease(_reaper_owner);
    });
}

void start_es_outbox_reaper() {
    _es_reaper_running = true;
    _es_reaper_thread = std::thread([this]() {
        while (_es_reaper_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!_es_reaper_election || !_es_reaper_election->is_leader())
                continue;
            try {
                auto items = _es_outbox->dequeue(50);
                for (const auto &item : items) {
                    _es_client->index(item.index, item.doc);
                    _es_outbox->ack(item.id);
                }
            } catch (std::exception &e) {
                LOG_WARN("ESOutbox reaper 异常: {}", e.what());
            }
        }
        _es_outbox->release_reaper_lease(_reaper_owner);
    });
}

// 新增私有成员:
std::thread _push_reaper_thread;
std::thread _es_reaper_thread;
std::atomic<bool> _push_reaper_running{false};
std::atomic<bool> _es_reaper_running{false};
```

在 `MessageServer::start()` 中 reaper 线程 join 与 election stop（见 Step 1 的 shutdown 路径）。

- [ ] **Step 1: Add LeaderElection support to MessageServerBuilder**

In `message/source/message_server.h`, add to `MessageServerBuilder`:

```cpp
void set_etcd_client(std::shared_ptr<etcd::Client> etcd) { _etcd_client = etcd; }

void make_reaper_elections() {
    if (!_etcd_client) {
        LOG_WARN("etcd 未初始化，跳过 reaper 选举");
        return;
    }
    _push_reaper_election = std::make_shared<LeaderElection>(
        _etcd_client, "/chatnow/reaper/push_outbox", _reaper_owner, 30,
        []() { LOG_INFO("PushOutbox reaper 成为 leader"); },
        []() { LOG_INFO("PushOutbox reaper 失去 leader"); });
    _es_reaper_election = std::make_shared<LeaderElection>(
        _etcd_client, "/chatnow/reaper/es_outbox", _reaper_owner, 30,
        []() { LOG_INFO("ESOutbox reaper 成为 leader"); },
        []() { LOG_INFO("ESOutbox reaper 失去 leader"); });
}

// Private members to add (reaper threads + running flags 已在 Step 0 添加):
std::shared_ptr<etcd::Client> _etcd_client;
LeaderElection::ptr _push_reaper_election;
LeaderElection::ptr _es_reaper_election;
```

Step 0 中创建的 reaper 线程已经使用 `_push_reaper_election->is_leader()` 做选主判断。Step 1 负责创建 `_push_reaper_election` / `_es_reaper_election` 对象并注入到 `MessageServerBuilder`。线程内逻辑无需再改动。

In `MessageServer::start()`, start elections and reaper threads:
```cpp
_push_reaper_election->start();
_es_reaper_election->start();
start_push_outbox_reaper();
start_es_outbox_reaper();
```

Stop in shutdown path (after reaper threads join, before election stop):
```cpp
_push_reaper_running = false;
_es_reaper_running = false;
if (_push_reaper_thread.joinable()) _push_reaper_thread.join();
if (_es_reaper_thread.joinable()) _es_reaper_thread.join();
_push_reaper_election->stop();
_es_reaper_election->stop();
```

- [ ] **Step 2: Wire etcd client in message_server.cc**

```cpp
msb.set_etcd_client(std::make_shared<etcd::Client>(FLAGS_registry_host));
msb.make_reaper_elections();
```

- [ ] **Step 3: Same treatment for Push CrossInstanceOutbox reaper**

In `push/source/push_server.h`, `PushServerBuilder`:

```cpp
void set_etcd_client(std::shared_ptr<etcd::Client> etcd) { _etcd_client = etcd; }

void make_cross_reaper_election() {
    if (!_etcd_client) return;
    _cross_reaper_election = std::make_shared<LeaderElection>(
        _etcd_client, "/chatnow/reaper/cross_outbox", _instance_id, 30,
        []() { LOG_INFO("CrossOutbox reaper 成为 leader"); },
        []() { LOG_INFO("CrossOutbox reaper 失去 leader"); });
}

std::shared_ptr<etcd::Client> _etcd_client;
LeaderElection::ptr _cross_reaper_election;
```

In the Push cross reaper thread, replace `try_acquire_reaper_lease` with `_cross_reaper_election->is_leader()`. Start the election before the cross reaper thread, stop it on shutdown.

- [ ] **Step 4: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add message/source/message_server.h message/source/message_server.cc \
        push/source/push_server.h push/source/push_server.cc
git commit -m "feat: migrate PushOutbox/ESOutbox/CrossInstanceOutbox reapers from Lua CAS to etcd LeaderElection"
```

---

### Task 8: Migrate Snowflake worker_id allocation to etcd LeaderElection

**Files:**
- Modify: `common/infra/snowflake.hpp`
- Modify: `transmite/source/transmite_server.h`
- Modify: `transmite/source/transmite_server.cc`

- [ ] **Step 1: Refactor WorkIdAllocator to use LeaderElection per slot**

In `common/infra/snowflake.hpp`, update `WorkIdAllocator` (or `SnowflakeIdGenerator`):

```cpp
#include "infra/leader_election.hpp"

// Replaces Redis SETNX-based worker_id allocation with etcd LeaderElection.
// Each worker_id slot (0-1023) maps to an etcd key /chatnow/snowflake/worker/{id}.
// Campaign on slot 0 first; if taken, try slot 1, etc.

class EtcdWorkIdAllocator {
public:
    EtcdWorkIdAllocator(std::shared_ptr<etcd::Client> etcd, int max_workers = 1024)
        : _etcd(std::move(etcd)), _max_workers(max_workers) {}

    int allocate(const std::string &instance_id, int lease_ttl = 60) {
        for (int slot = 0; slot < _max_workers; ++slot) {
            auto key = "/chatnow/snowflake/worker/" + std::to_string(slot);
            auto election = std::make_shared<LeaderElection>(
                _etcd, key, instance_id, lease_ttl, nullptr, nullptr);
            election->start();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (election->is_leader()) {
                _active_election = election;
                return slot;
            }
            election->stop();
        }
        LOG_ERROR("EtcdWorkIdAllocator: 无可用 worker_id slot");
        return -1;
    }

    void deallocate() { if (_active_election) _active_election->stop(); }

    bool lease_lost() {
        return _active_election && !_active_election->is_leader();
    }

private:
    std::shared_ptr<etcd::Client> _etcd;
    int _max_workers;
    LeaderElection::ptr _active_election;
};
```

Note: Keep the existing Redis-based `WorkIdAllocator` as a fallback — it still works for single-instance Redis mode. The Transmite builder selects between etcd and Redis mode based on `--redis_seeds` being set (Cluster mode → etcd, single mode → Redis fallback).

- [ ] **Step 2: Wire in TransmiteServerBuilder**

```cpp
void set_etcd_client(std::shared_ptr<etcd::Client> etcd) { _etcd_client = etcd; }

void make_snowflake_id_generator() {
    if (_etcd_client && !_redis_seeds.empty()) {
        _id_generator = std::make_shared<SnowflakeIdGenerator>(
            std::make_shared<EtcdWorkIdAllocator>(_etcd_client));
    } else {
        // existing Redis-based WorkIdAllocator
        _id_generator = std::make_shared<SnowflakeIdGenerator>(
            std::make_shared<WorkIdAllocator>(_redis_client));
    }
}
```

- [ ] **Step 3: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add common/infra/snowflake.hpp transmite/source/transmite_server.h transmite/source/transmite_server.cc
git commit -m "feat: migrate Snowflake worker_id allocation to etcd LeaderElection"
```

---

### Task 9: Add RedisMutex to SeqGen backfill for multi-instance startup coordination

**Files:**
- Modify: `message/source/message_server.h`

- [ ] **Step 1: Wrap backfill with RedisMutex**

```cpp
#include "utils/redis_mutex.hpp"

void backfill_seq_from_db_() {
    if (!_seq_gen || !_odb_db) {
        LOG_WARN("SeqGen / MySQL 未初始化，跳过 seq 回填");
        return;
    }

    // 多实例启动互斥：同一时间只有一个实例执行 backfill
    RedisMutex backfill_lock(_redis_client, "backfill:seq", 30000);
    if (!backfill_lock.try_lock(std::chrono::seconds(5))) {
        LOG_WARN("SeqGen backfill 获取锁超时（其他实例正在执行），跳过");
        return;
    }

    LOG_INFO("开始从 DB 回填 seq 到 Redis...");
    auto msg_table = std::make_shared<MessageTable>(_odb_db);
    auto timeline_table = std::make_shared<UserTimeLineTable>(_odb_db);

    auto session_seqs = msg_table->select_max_seq_by_session();
    for (const auto &[ssid, max_seq] : session_seqs) {
        if (max_seq > 0) _seq_gen->backfill_session(ssid, max_seq + 1);
    }
    LOG_INFO("回填 session_seq 完成: {} 个会话", session_seqs.size());

    auto user_seqs = timeline_table->select_max_user_seq();
    for (const auto &[uid, max_seq] : user_seqs) {
        if (max_seq > 0) _seq_gen->backfill_user(uid, max_seq + 1);
    }
    LOG_INFO("回填 user_seq 完成: {} 个用户", user_seqs.size());

    backfill_lock.unlock();
}
```

- [ ] **Step 2: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add message/source/message_server.h
git commit -m "feat: add RedisMutex around SeqGen backfill for multi-instance coordination"
```

---

## Phase 3: L1 Local Cache

### Task 10: Create LocalCache<T> template

**Files:**
- Create: `common/utils/local_cache.hpp`

- [ ] **Step 1: Write LocalCache<T>**

```cpp
// common/utils/local_cache.hpp
#pragma once

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 线程安全的进程内本地缓存。TTL 自动过期（lazy eviction），读写锁保护。
template <typename V>
class LocalCache {
public:
    using ptr = std::shared_ptr<LocalCache<V>>;

    explicit LocalCache(size_t size_hint = 4096) { _map.reserve(size_hint); }

    std::optional<V> get(const std::string &key) {
        std::shared_lock lk(_mu);
        auto it = _map.find(key);
        if (it == _map.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() > it->second.expires_at)
            return std::nullopt;
        return it->second.value;
    }

    void set(const std::string &key, const V &value, std::chrono::seconds ttl) {
        std::unique_lock lk(_mu);
        _map[key] = {value, std::chrono::steady_clock::now() + ttl};
    }

    // CAS: 仅当 key 不存在或已过期时设置（防击穿——第一个 miss 的请求 set，后续直接 get）
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

    size_t size() const {
        std::shared_lock lk(_mu);
        return _map.size();
    }

    size_t evict_expired() {
        std::unique_lock lk(_mu);
        auto now = std::chrono::steady_clock::now();
        size_t removed = 0;
        for (auto it = _map.begin(); it != _map.end(); ) {
            if (now > it->second.expires_at) { it = _map.erase(it); ++removed; }
            else { ++it; }
        }
        return removed;
    }

private:
    struct Entry {
        V value;
        std::chrono::steady_clock::time_point expires_at;
    };
    mutable std::shared_mutex _mu;
    std::unordered_map<std::string, Entry> _map;
};

} // namespace chatnow
```

- [ ] **Step 2: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add common/utils/local_cache.hpp
git commit -m "feat: add LocalCache<T> template for L1 in-process caching"
```

---

### Task 11: Create InflightRegistry (per-key process-internal mutex for stampede protection)

**Files:**
- Create: `common/utils/inflight.hpp`

**Why:** 当多个并发请求同时 miss L1 和 L2 缓存时，需要合并为一个穿透请求。InflightRegistry 提供 per-key 进程内互斥 + double-check 机制——第一个 miss 的请求获取锁并穿透，后续请求在锁上等待，锁释放后从 L1 读取。这是 Spec `2026-05-13-cache-strategy-redesign.md` §3 中定义但尚未实现的组件。

- [ ] **Step 1: Write InflightRegistry**

```cpp
// common/utils/inflight.hpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 进程内 per-key 互斥注册表：用于合并同一 key 的并发缓存穿透请求。
// 第一个 miss 的请求 acquire(key) 获取互斥锁，unique_lock 锁定后穿透后端，
// warm 缓存，然后 release(key)。后续相同 key 的请求 acquire() 拿到同一个
// mutex，在 unique_lock 上阻塞直到第一个请求完成并 unlock。
//
// 线程安全。
class InflightRegistry {
public:
    using ptr = std::shared_ptr<InflightRegistry>;

    // 为给定 key 获取（或创建）互斥锁。返回未锁定的 mutex——调用者负责
    // std::unique_lock 锁定，完成 L2→RPC→warm 后 unlock + release。
    struct Guard {
        std::shared_ptr<std::mutex> mu;
        std::string key;
        InflightRegistry *registry;
    };

    Guard acquire(const std::string &key) {
        std::shared_ptr<std::mutex> mu;
        {
            std::lock_guard lk(_mu);
            auto it = _inflight.find(key);
            if (it == _inflight.end()) {
                mu = std::make_shared<std::mutex>();
                _inflight[key] = mu;
            } else {
                mu = it->second;
            }
        }
        return {mu, key, this};
    }

    // 释放 key 的注册（应在 unlock 之后调用）
    void release(const std::string &key) {
        std::lock_guard lk(_mu);
        _inflight.erase(key);
    }

private:
    std::mutex _mu;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> _inflight;
};

} // namespace chatnow
```

Usage pattern (used by Task 12 and Task 13):
```cpp
auto guard = _inflight_registry->acquire(key);
std::unique_lock lk(*guard.mu);
// double-check L1 → double-check L2 → RPC → warm L2 → warm L1
lk.unlock();
guard.registry->release(guard.key);
```

- [ ] **Step 2: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add common/utils/inflight.hpp
git commit -m "feat: add InflightRegistry for per-key in-process stampede protection"
```

---

### Task 12: Integrate L1 OnlineRoute cache in Push service

**Files:**
- Modify: `push/source/push_server.h`

- [ ] **Step 1: Add RouteEntry struct and L1 cache to PushServerBuilder**

```cpp
#include "utils/local_cache.hpp"

struct RouteEntry {
    std::vector<std::string> device_ids;
    std::unordered_map<std::string, std::string> device_to_instance; // device_id → instance
};

// In PushServerBuilder:
void make_local_cache() {
    _local_route_cache = std::make_shared<LocalCache<RouteEntry>>(16384);
    _inflight_registry = std::make_shared<InflightRegistry>();
}

LocalCache<RouteEntry>::ptr _local_route_cache;   // add as private member
InflightRegistry::ptr _inflight_registry;          // add as private member
```

- [ ] **Step 2: Rewrite OnlineRoute read path with L1 → InflightRegistry double-check → L2 fallback**

In `PushServiceImpl::onPushMessage`, where `_online_route->devices(uid)` and `_online_route->device_instance(uid, did)` are called, replace with:

```cpp
#include "utils/random_ttl.hpp"
#include "utils/inflight.hpp"

RouteEntry resolve_route(const std::string &uid) {
    std::string cache_key = "route:" + uid;

    // ① L1 hit → fast path (~ns)
    auto cached = _local_route_cache->get(cache_key);
    if (cached.has_value()) return *cached;

    // ② L1 miss → acquire InflightRegistry per-key lock
    auto guard = _inflight_registry->acquire(cache_key);
    std::unique_lock lk(*guard.mu);

    // ③ Double-check L1 (another thread may have just finished warm)
    cached = _local_route_cache->get(cache_key);
    if (cached.has_value()) {
        lk.unlock();
        guard.registry->release(guard.key);
        return *cached;
    }

    // ④ L2 Redis: hgetall + build RouteEntry
    RouteEntry route;
    auto devices = _online_route->devices(uid);
    route.device_ids = std::move(devices);
    for (const auto &did : route.device_ids) {
        route.device_to_instance[did] = _online_route->device_instance(uid, did);
    }
    _local_route_cache->set(cache_key, route, randomized_ttl(std::chrono::seconds(2)));

    lk.unlock();
    guard.registry->release(guard.key);
    return route;
}
// Use route.device_ids and route.device_to_instance for the rest of the handler
```

Inject `_local_route_cache` and `_inflight_registry` into `PushServiceImpl` via constructor (update `make_rpc_object`).

- [ ] **Step 3: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add push/source/push_server.h
git commit -m "feat: add L1 local cache for OnlineRoute in Push service"
```

---

### Task 13: Integrate L1 Members + UserInfo cache in Transmite with InflightRegistry + RedisMutex

**Files:**
- Modify: `transmite/source/transmite_server.h`

> **依赖**: `InflightRegistry` 已在 Task 11 中创建（`common/utils/inflight.hpp`），`LocalCache<T>` 已在 Task 10 中创建。本 Task 直接引用这两个组件。

- [ ] **Step 1: Add L1 caches + InflightRegistry to TransmiteServerBuilder**

```cpp
#include "utils/local_cache.hpp"
#include "utils/redis_mutex.hpp"
#include "utils/random_ttl.hpp"

// In TransmiteServerBuilder:
void make_local_cache() {
    _local_members_cache = std::make_shared<LocalCache<std::vector<std::string>>>(4096);
    _local_user_cache = std::make_shared<LocalCache<std::string>>(16384);
    _inflight_registry = std::make_shared<InflightRegistry>();
}

LocalCache<std::vector<std::string>>::ptr _local_members_cache;
LocalCache<std::string>::ptr _local_user_cache;
InflightRegistry::ptr _inflight_registry;
```

- [ ] **Step 2: Rewrite Members read path with L1 → InflightRegistry double-check → L2 → RedisMutex → RPC**

Where `resolve_members(ssid)` runs, replace the existing logic with:

```cpp
std::vector<std::string> resolve_members(const std::string &chat_session_id) {
    std::string mkey = "members:" + chat_session_id;

    // ① L1 hit → fast path (~ns)
    auto local = _local_members_cache->get(mkey);
    if (local.has_value()) {
        auto &members = *local;
        if (members.size() == 1 && members[0] == "__sentinel__")
            return {};  // known non-existent session
        return members;
    }

    // ② L1 miss → acquire InflightRegistry per-key lock
    auto guard = _inflight_registry->acquire(chat_session_id);
    std::unique_lock lk(*guard.mu);

    // ③ Double-check L1 (another thread may have just finished warm)
    local = _local_members_cache->get(mkey);
    if (local.has_value()) {
        lk.unlock();
        guard.registry->release(guard.key);
        auto &members = *local;
        if (members.size() == 1 && members[0] == "__sentinel__") return {};
        return members;
    }

    // ④ Double-check L2 Redis
    auto members = _members_cache->list(chat_session_id);
    if (!members.empty()) {
        if (members.size() == 1 && members[0] == "__sentinel__") {
            _local_members_cache->set(mkey, {"__sentinel__"}, randomized_ttl(std::chrono::seconds(60)));
            lk.unlock();
            guard.registry->release(guard.key);
            return {};
        }
        _local_members_cache->set(mkey, members, randomized_ttl(std::chrono::seconds(8)));
        lk.unlock();
        guard.registry->release(guard.key);
        return members;
    }

    // ⑤ L2 miss → cross-instance RedisMutex (prevent multi-instance stampede)
    RedisMutex warm_mutex(_redis_client, "warm:members:" + chat_session_id, 5000);
    if (!warm_mutex.try_lock(std::chrono::milliseconds(100))) {
        // Another instance is warming — wait briefly and retry L1
        lk.unlock();
        guard.registry->release(guard.key);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return resolve_members(chat_session_id);
    }

    // ⑥ RPC ChatSession.GetMemberIdList
    members = /* existing RPC call */;
    if (members.empty()) {
        // Sentinel: cache empty result to prevent penetration
        _members_cache->warm_sentinel(chat_session_id);
        _local_members_cache->set(mkey, {"__sentinel__"}, randomized_ttl(std::chrono::seconds(60)));
    } else {
        _members_cache->warm(chat_session_id, members);
        _local_members_cache->set(mkey, members, randomized_ttl(std::chrono::seconds(8)));
    }

    warm_mutex.unlock();
    lk.unlock();
    guard.registry->release(guard.key);
    return members;
}
```

- [ ] **Step 3: Rewrite UserInfo read path with L1 → L2 → RPC**

```cpp
std::string resolve_user_info(const std::string &uid) {
    std::string ukey = "user:" + uid;
    auto cached = _local_user_cache->get(ukey);
    if (cached.has_value()) return *cached;

    // L2: UserInfoCache (added by prior spec)
    auto info = _user_cache->get(uid);
    if (info.has_value()) {
        std::string serialized;
        info->SerializeToString(&serialized);
        _local_user_cache->set(ukey, serialized, randomized_ttl(std::chrono::seconds(45)));
        return serialized;
    }

    // L3: RPC GetUserInfo
    // ... existing RPC call ...
    std::string serialized;
    user_info.SerializeToString(&serialized);
    _user_cache->batch_set({{uid, user_info}});
    _local_user_cache->set(ukey, serialized, randomized_ttl(std::chrono::seconds(45)));
    return serialized;
}
```

- [ ] **Step 4: Inject new dependencies into TransmiteServiceImpl constructor**

Update `make_rpc_object()` to pass `_local_members_cache`, `_local_user_cache`, `_inflight_registry`, `_redis_client`.

- [ ] **Step 5: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add transmite/source/transmite_server.h
git commit -m "feat: add L1 Members/UserInfo cache with InflightRegistry + RedisMutex stampede protection"
```

---

### Task 14: Push shutdown OnlineRoute cleanup + full stale route reaper

**Files:**
- Modify: `push/source/push_server.h`

- [ ] **Step 0: Add `connections()` accessor to PushServiceImpl**

> **注意 (未验证假设)**: 本 Step 假设 `PushServiceImpl` 中已有 `_connections` 成员（类型为按 uid 索引的连接容器）。实施前需确认该成员的准确名称和类型。

`_connections` 是 `PushServiceImpl` 的私有成员，`PushServer` 需要访问它来做关停清理。在 `PushServiceImpl` 中新增：

```cpp
// push/source/push_server.h — PushServiceImpl 新增 public 方法:
const auto& connections() const { return _connections; }
```

- [ ] **Step 1: Add graceful shutdown cleanup in PushServer::start()**

After `_ws_server->stop()` (around line 663 in push_server.h), add:

```cpp
// 遍历所有连接，主动清理 OnlineRoute 和 L1 缓存
LOG_INFO("Push 关停: 开始清理 OnlineRoute...");
for (const auto &[uid, conn] : _service_impl->connections()) {
    auto devices = _online_route->devices(uid);
    for (const auto &device_id : devices) {
        _online_route->unbind(uid, device_id, _service_impl->_instance_id);
    }
    _local_route_cache->invalidate("route:" + uid);
}
LOG_INFO("Push 关停: OnlineRoute + L1 缓存已清理");
```

- [ ] **Step 2: Implement stale route reaper with LeaderElection + etcd ls + Redis SCAN**

在 `PushServiceImpl` 中新增方法，并在 `PushServer::start()` 中以独立线程启动：

```cpp
void reap_stale_routes_() {
    // 受 LeaderElection 保护，只有 leader 执行清理
    while (_running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!_stale_reaper_election || !_stale_reaper_election->is_leader())
            continue;

        // 1. 从 etcd 获取当前在线 Push 实例列表（匹配现有 Discovery 的 ls 模式）
        std::vector<std::string> online_instances;
        auto resp = _etcd_client->ls(_push_service_dir).get();  // e.g. "/chatnow/services/push/"
        if (resp.is_ok()) {
            for (int i = 0; i < static_cast<int>(resp.keys().size()); ++i) {
                online_instances.push_back(resp.value(i).as_string());
            }
        }

        // 2. SCAN im:online:* 并按实例分组
        std::vector<std::pair<std::string, std::string>> stale_entries; // (uid, device_id)
        long long cursor = 0;
        do {
            std::vector<std::string> keys;
            cursor = _redis_client->scan(cursor, "im:online:*", 100, std::back_inserter(keys));
            for (const auto &key : keys) {
                // key格式: "im:online:{uid}"
                std::string uid = key.substr(std::string("im:online:").size());
                std::unordered_map<std::string, std::string> device_map;
                _redis_client->hgetall(key, std::inserter(device_map, device_map.end()));
                for (const auto &[did, instance] : device_map) {
                    if (std::find(online_instances.begin(), online_instances.end(), instance)
                        == online_instances.end()) {
                        stale_entries.emplace_back(uid, did);
                    }
                }
            }
        } while (cursor != 0);

        // 3. HDEL 清理僵死路由
        for (const auto &[uid, did] : stale_entries) {
            _online_route->unbind(uid, did, ""); // instance 参数不使用
            _local_route_cache->invalidate("route:" + uid);
        }
        if (!stale_entries.empty()) {
            LOG_INFO("僵死路由清理: 移除 {} 条记录", stale_entries.size());
        }
    }
}
```

**注入路径（PushServerBuilder → PushServiceImpl）**：

> **注意**: `set_etcd_client()` 和 `_etcd_client` 成员已在 Task 7（CrossInstanceOutbox reaper 迁移）中添加。本 Task 只需新增以下内容。

```cpp
// PushServerBuilder 新增（set_etcd_client 和 _etcd_client 已在 Task 7 添加）:
void set_push_service_dir(const std::string &dir) { _push_service_dir = dir; }

void make_stale_reaper_election() {
    if (!_etcd_client) return;
    _stale_reaper_election = std::make_shared<LeaderElection>(
        _etcd_client, "/chatnow/reaper/stale_routes", _instance_id, 30,
        []() { LOG_INFO("StaleRoute reaper 成为 leader"); },
        []() { LOG_INFO("StaleRoute reaper 失去 leader"); });
}

// 在 make_rpc_object() 中将依赖注入 PushServiceImpl 构造函数:
//   _etcd_client, _push_service_dir, _stale_reaper_election,
//   _redis_client, _online_route, _local_route_cache

std::string _push_service_dir;  // e.g. "/chatnow/services/push/"
LeaderElection::ptr _stale_reaper_election;
```

```cpp
// PushServiceImpl 构造函数新增参数:
PushServiceImpl(
    // ... 现有参数 ...
    std::shared_ptr<etcd::Client> etcd_client,
    const std::string &push_service_dir,
    LeaderElection::ptr stale_reaper_election,
    RedisClient::ptr redis_client,
    OnlineRoute::ptr online_route,
    LocalCache<RouteEntry>::ptr local_route_cache)
    : /* ... */
      _etcd_client(std::move(etcd_client)),
      _push_service_dir(push_service_dir),
      _stale_reaper_election(std::move(stale_reaper_election)),
      _redis_client(std::move(redis_client)),
      _online_route(std::move(online_route)),
      _local_route_cache(std::move(local_route_cache)) {}

// PushServiceImpl 新增私有成员:
std::shared_ptr<etcd::Client> _etcd_client;
std::string _push_service_dir;
LeaderElection::ptr _stale_reaper_election;
RedisClient::ptr _redis_client;
OnlineRoute::ptr _online_route;
LocalCache<RouteEntry>::ptr _local_route_cache;
```

在 `PushServer::start()` 中启动 reaper:
```cpp
_stale_reaper_election->start();
auto stale_thread = std::thread([this]() { _service_impl->reap_stale_routes_(); });

// ... shutdown path:
_service_impl->_running = false;
stale_thread.join();
_stale_reaper_election->stop();
```

**替代方案（更简单）**：如果不需要跨实例选主，也可以直接用 etcd `ls()` 的结果做实例存活判断，省掉 etcd 的额外依赖——Push 服务本身已经通过 `common/infra/etcd.hpp` 注册了自己，直接复用 `_etcd_client->ls()` 即可。

- [ ] **Step 3: Reduce OnlineRoute TTL from 120s to 30s**

In `common/dao/data_redis.hpp`, line 67:

```cpp
// Change:
inline constexpr std::chrono::seconds kOnlineTtl(120);
// To:
inline constexpr std::chrono::seconds kOnlineTtl(30);
```

- [ ] **Step 4: Verify heartbeat interval ≤15s**

In `PushServiceImpl::onClientNotify` (HEARTBEAT case), verify `_online_route->touch(uid)` is called on every heartbeat. If current heartbeat interval > 15s, adjust the client-side heartbeat period or set `kOnlineTtl` to `2 * heartbeat_interval + 5s`.

- [ ] **Step 5: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add push/source/push_server.h common/dao/data_redis.hpp
git commit -m "feat: shutdown cleanup + full stale route reaper with etcd LeaderElection + OnlineRoute TTL 30s"
```

---

## Phase 4: Cache Protection

### Task 15: Create `random_ttl.hpp` and apply to all cache TTL writes

**Files:**
- Create: `common/utils/random_ttl.hpp`
- Modify: `common/dao/data_redis.hpp` (all `set`/`expire` calls with TTL)
- Modify: `push/source/push_server.h` (L1 set calls)
- Modify: `transmite/source/transmite_server.h` (L1 set calls)

- [ ] **Step 1: Write randomized_ttl()**

```cpp
// common/utils/random_ttl.hpp
#pragma once
#include <chrono>
#include <random>

namespace chatnow {
inline std::chrono::seconds randomized_ttl(std::chrono::seconds base) {
    long base_sec = base.count();
    long jitter = base_sec / 5;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<long> dist(-jitter, jitter);
    return std::chrono::seconds(base_sec + dist(rng));
}
} // namespace chatnow
```

- [ ] **Step 2: Apply to cache-type TTL writes in data_redis.hpp**

Add `#include "utils/random_ttl.hpp"` at the top of `data_redis.hpp`.

For each `_c->set(key, val, ttl)` where `ttl` is a cache TTL (not a data retention TTL), replace with `_c->set(key, val, randomized_ttl(ttl))`. Same for `_c->expire(key, ttl)`.

Apply to these cache-type classes:
- `Members::warm` (line ~410) — `_c->expire(k, randomized_ttl(ttl))`
- `Members::touch_ttl` (new method) — `_c->expire(k, randomized_ttl(ttl))`
- `Members::warm_sentinel` — `_c->expire(k, randomized_ttl(ttl))`
- `OnlineRoute::bind` (line ~450) — `_c->expire(k, randomized_ttl(ttl))`
- `OnlineRoute::touch` (line ~458) — `_c->expire(k, randomized_ttl(ttl))`
- `LastMessage::set` (line ~285) — `_c->set(key, val, randomized_ttl(ttl))`
- `ReadAck::ack` (line ~353) — `_c->expire(k, randomized_ttl(ttl))`

Do NOT randomize:
- `Session::append` (7-day data TTL — jitter of ±1.4 days is harmful)
- `Status::append` (5-min data TTL — online status integrity)
- `UnackedPush::push` (7-day retransmission buffer — must be predictable)
- `Codes::append` (5-min verification code — security-sensitive)

- [ ] **Step 3: Apply to L1 set calls in push_server.h and transmite_server.h**

All `_local_*_cache->set(key, val, ttl)` calls already use `randomized_ttl(ttl)` (wired in Tasks 11-12).

- [ ] **Step 4: Commit**

```bash
git add common/utils/random_ttl.hpp common/dao/data_redis.hpp \
        push/source/push_server.h transmite/source/transmite_server.h
git commit -m "feat: randomized TTL on cache writes (not data-retention keys) to prevent avalanche"
```

---

### Task 16: Add sentinel null-cache for Members cache penetration prevention

**Files:**
- Modify: `common/dao/data_redis.hpp` (Members class)

- [ ] **Step 1: Add `warm_sentinel()` and `touch_ttl()` to Members class**

```cpp
// In class Members (after line ~428):

void touch_ttl(const std::string &ssid, std::chrono::seconds ttl = kMembersTtl) {
    try { _c->expire(key::kMembers + ssid, randomized_ttl(ttl)); }
    catch (std::exception &e) { LOG_ERROR("Members.touch_ttl 失败 {}: {}", ssid, e.what()); }
}

void warm_sentinel(const std::string &ssid, std::chrono::seconds ttl = std::chrono::seconds(60)) {
    try {
        std::string k = key::kMembers + ssid;
        _c->sadd(k, "__sentinel__");
        _c->expire(k, randomized_ttl(ttl));
    } catch (std::exception &e) { LOG_ERROR("Members.warm_sentinel 失败 {}: {}", ssid, e.what()); }
}
```

- [ ] **Step 2: Sentinel check integrated in Task 13's read path**

Task 13 step 2 already includes sentinel checking in the Members read path. No additional changes needed — the `"__sentinel__"` check is embedded in `resolve_members()`.

- [ ] **Step 3: Build verify & commit**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
git add common/dao/data_redis.hpp
git commit -m "feat: sentinel null-cache (warm_sentinel) for Members penetration prevention"
```

---

## Phase 5: Monitoring

### Task 17: Add Prometheus alert rules

**Files:**
- Create: `scripts/prometheus/redis_alerts.yml`

- [ ] **Step 1: Write alert rules**

```yaml
# scripts/prometheus/redis_alerts.yml
groups:
  - name: redis_cluster
    rules:
      - alert: RedisNodeDown
        expr: redis_up{node=~".+"} == 0
        for: 1m
        labels: { severity: critical }
        annotations:
          summary: "Redis node {{ $labels.node }} down"
          description: "Redis Cluster node {{ $labels.node }} has been down for >1m"

      - alert: RedisPoolExhausted
        expr: redis_pool_active >= redis_pool_size
        for: 1m
        labels: { severity: critical }
        annotations:
          summary: "Redis connection pool exhausted on {{ $labels.instance }}"
          description: "redis_pool_active ({{ $value }}) >= pool_size, connections saturated"

      - alert: LeaderElectionFrequentChange
        expr: rate(leader_election_changes_total[10m]) > 2
        for: 1m
        labels: { severity: warning }
        annotations:
          summary: "Leader election changing frequently (>2x in 10min)"

      - alert: LocalCacheHitRateDrop
        expr: rate(local_cache_miss_total[5m]) > 3 * rate(local_cache_miss_total[5m] offset 30m)
        for: 5m
        labels: { severity: warning }
        annotations:
          summary: "Local cache miss rate spiked 3x vs 30min ago"
```

- [ ] **Step 2: Commit**

```bash
git add scripts/prometheus/
git commit -m "feat: add Prometheus alert rules for Redis Cluster + LeaderElection + L1 cache"
```

---

## Verification Checklist

After all Phases complete:

### Redis Cluster
- [ ] `redis-cli -h <seed> cluster info` → 3 masters + 3 slaves
- [ ] Kill one master → slave promotes within 5s
- [ ] Full cluster restart → SeqGen backfill correct, no seq collision

### Distributed Locks
- [ ] etcd reaper election: kill leader → backup takes over within lease_ttl (30s)
- [ ] No double-leader: two instances concurrently start → exactly one wins (Transaction CAS)
- [ ] RedisMutex: kill holder → TTL expires → another instance acquires
- [ ] Snowflake worker_id: two instances → different worker_ids assigned

### L1 Local Cache
- [ ] L1 hit → no Redis access (verify via log counters)
- [ ] L1 miss → InflightRegistry coalesces concurrent requests → single RPC
- [ ] L1 miss + L2 miss + multi-instance → RedisMutex prevents simultaneous RPC storm
- [ ] Push OnlineRoute L1 hit rate > 90% under load

### Online Route & Shutdown
- [ ] Push crash → stale reaper cleans route within 30s
- [ ] Push graceful shutdown → active unbind + L1 invalidation within 1s
- [ ] User switches Push instance → new route effective, old TTL expires

### Cache Protection
- [ ] Non-existent session → sentinel cached → no further RPC for 60s
- [ ] Service restart → TTLs spread across ±20% range (no simultaneous expiry)
- [ ] Data-retention TTLs (Session, UnackedPush) NOT randomized

---

## Summary

| Phase | Tasks | Key deliverables |
|-------|-------|-----------------|
| 1: Redis Cluster | 4 | RedisClient adapter, dual-mode init (8 services), 6-node docker-compose Cluster |
| 2: Distributed Locks | 5 | LeaderElection (etcd Transaction CAS), RedisMutex, PushOutbox/ESOutbox reaper threads created + all 3 reapers on LeaderElection + Snowflake + backfill mutex |
| 3: L1 Local Cache | 5 | LocalCache<T>, InflightRegistry, OnlineRoute L1 with InflightRegistry, Members L1 with InflightRegistry+RedisMutex, shutdown cleanup + full stale reaper |
| 4: Cache Protection | 2 | randomized_ttl (cache keys only), sentinel null-cache |
| 5: Monitoring | 1 | Prometheus alert rules for Redis + locks + L1 cache hit rate |
| **Total** | **17** | |
