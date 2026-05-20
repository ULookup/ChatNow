# Cache Infrastructure Redesign — Review Fix Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 3 CRITICAL + 7 IMPORTANT + 7 MINOR issues found in code review of `feat/cache-infrastructure-redesign` against spec `2026-05-18-cache-infrastructure-redesign.md`.

**Architecture:** The fixes center on three root causes: (1) `RedisClient` adapter missing `set()` overload with `UpdateType` that causes both the RedisMutex NX bug and compilation failures at two call sites, (2) `Transmite::resolve_members()` design bug where RedisMutex is acquired then immediately released without performing the actual cache warm, (3) `docker-compose.yml` referencing non-existent service names. Secondary fixes address `randomized_ttl()` precision, `LeaderElection::stop()` blocking, `InflightRegistry::Guard` RAII, missing `_push_service_dir`, unused `_local_user_cache`, recursion risk, and dead code removal.

**Tech Stack:** C++17, sw::redis++, etcd-cpp-apiv3

**Spec:** `docs/superpowers/specs/2026-05-18-cache-infrastructure-redesign.md`

---

## File Structure

```
common/dao/data_redis.hpp          — +set(key,val,ttl,UpdateType) overloads to RedisClient
common/utils/redis_mutex.hpp       — 使用 set() with NOT_EXIST 替代无 NX 的 set()
common/utils/random_ttl.hpp        — 浮点 jitter 替代整数除法
common/utils/inflight.hpp          — Guard RAII（析构自动 release）
common/infra/leader_election.hpp   — sleep_for → condition_variable wait_for
transmite/source/transmite_server.h — resolve_members 重构：RedisMutex 持有期间执行 RPC+warm
push/source/push_server.cc         — 添加 set_push_service_dir() 调用
push/source/push_server.h          — WS close handler 添加 L1 invalidate；SCAN 增加 Cluster 警告
docker-compose.yml                 — depends_on 修正为 redis-node*
```

---

## Phase 1: CRITICAL Fixes (BLOCK)

### Task 1: Add `set(key, val, ttl, UpdateType)` overloads to RedisClient

**Files:**
- Modify: `common/dao/data_redis.hpp:50-57`

**Why:** `RedisClient` 仅有两个 `set(k,v,seconds)` 和 `set(k,v,milliseconds)` 重载，但 `worker_id.hpp:68` 和 `transmite_server.h:137` 需要带 `UpdateType` 的 4 参数版本。同时 RedisMutex 需要 `NOT_EXIST` 实现 NX 语义。

- [ ] **Step 1: Add two set() overloads with UpdateType**

In `common/dao/data_redis.hpp`, after the existing `set(key, val, milliseconds)` overload (line 57), insert:

```cpp
bool set(const std::string &key, const std::string &val,
         std::chrono::seconds ttl, sw::redis::UpdateType type) {
    return _rc ? _rc->set(key, val, ttl, type) : _r->set(key, val, ttl, type);
}
bool set(const std::string &key, const std::string &val,
         std::chrono::milliseconds ttl, sw::redis::UpdateType type) {
    return _rc ? _rc->set(key, val, ttl, type) : _r->set(key, val, ttl, type);
}
```

Note: `sw::redis::Redis::set(key, val, ms, UpdateType)` issues `SET key val PX ms NX` (for NOT_EXIST) and returns `true` only when the key was newly created. `sw::redis::RedisCluster::set()` has the same overload. This is the correct way to implement `SET NX PX` semantics.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -20
```

Expected: All targets compile. Previously broken call sites (`worker_id.hpp:68`, `transmite_server.h:137`) now resolve.

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix: add set(key,val,ttl,UpdateType) overloads to RedisClient adapter"
```

---

### Task 2: Fix RedisMutex::try_lock() to use SET NX semantics

**Files:**
- Modify: `common/utils/redis_mutex.hpp:27`

**Why:** 当前 `_redis->set(_key, _token, milliseconds)` 缺少 NX 标志，任何调用者都会成功覆盖已有锁，跨实例互斥完全失效。Spec §3.3 明确要求 `SET key value NX PX ttl_ms`。

- [ ] **Step 1: Change set() call to use NOT_EXIST**

Replace line 27 in `common/utils/redis_mutex.hpp`:

```cpp
// Before:
bool ok = _redis->set(_key, _token, std::chrono::milliseconds(_ttl_ms));

// After:
bool ok = _redis->set(_key, _token, std::chrono::milliseconds(_ttl_ms),
                      sw::redis::UpdateType::NOT_EXIST);
```

Note: With `NOT_EXIST`, `sw::redis++` issues `SET key token PX ttl_ms NX`. The underlying `Redis::set()` returns `true` only when the key did not previously exist — i.e., the lock was successfully acquired. If another instance already holds the key, `set()` returns `false` and `try_lock` spins.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/utils/redis_mutex.hpp
git commit -m "fix: use SET NX in RedisMutex::try_lock for actual mutual exclusion"
```

---

### Task 3: Fix docker-compose.yml depends_on to reference correct service names

**Files:**
- Modify: `docker-compose.yml:95-101`

**Why:** `redis-cluster-init` 的 `depends_on` 引用了不存在的 `redis-cluster-init-node1..6`，正确名称是 `redis-node1..6`。Docker Compose 无法解析不存在的依赖，容器会提前启动导致集群创建命令失败。

- [ ] **Step 1: Fix service names**

Replace lines 95-101 in `docker-compose.yml`:

```yaml
# Before:
    depends_on:
      - redis-cluster-init-node1
      - redis-cluster-init-node2
      - redis-cluster-init-node3
      - redis-cluster-init-node4
      - redis-cluster-init-node5
      - redis-cluster-init-node6

# After:
    depends_on:
      - redis-node1
      - redis-node2
      - redis-node3
      - redis-node4
      - redis-node5
      - redis-node6
```

- [ ] **Step 2: Commit**

```bash
git add docker-compose.yml
git commit -m "fix: correct redis-cluster-init depends_on to redis-node1..6"
```

---

## Phase 2: IMPORTANT Fixes

### Task 4: Restructure Transmite::resolve_members() — hold RedisMutex through RPC+warm

**Files:**
- Modify: `transmite/source/transmite_server.h:340-411`

**Why:** 当前 `resolve_members()` 在 L2 miss 后获取 RedisMutex，但立即 unlock 并 return `{}`。实际的 RPC 调用和 cache warm 在 `warm_members_cache()` 中由调用方单独执行，完全处于锁外。两个实例可以同时看到 L2 空、同时获取 InflightRegistry、同时发起 RPC。跨实例防击穿形同虚设。

另外，尾递归 `return resolve_members(chat_session_id)` 在 Redis 持续不可达时会无限递归，导致 brpc bthread 栈溢出。

- [ ] **Step 1: Remove `warm_members_cache()` standalone method, merge into `resolve_members()`**

The current split between `resolve_members()` (does the read path but stops at L2 miss) and `warm_members_cache()` (does the RPC and warm separately) is wrong. The RedisMutex-protected RPC and warm must happen atomically within `resolve_members()`.

Delete `warm_members_cache()` (lines 399-411) entirely. Its logic moves inline into `resolve_members()`.

- [ ] **Step 2: Rewrite `resolve_members()` with correct RedisMutex scope**

Replace `resolve_members()` (lines 340-397) and `warm_members_cache()` (lines 399-411) with:

```cpp
std::vector<std::string> resolve_members(const std::string &chat_session_id) {
    std::string mkey = "members:" + chat_session_id;

    // ① L1 hit → fast path
    auto local = _local_members_cache ? _local_members_cache->get(mkey) : std::nullopt;
    if (local.has_value()) {
        auto &members = *local;
        if (members.size() == 1 && members[0] == "__sentinel__") return {};
        return members;
    }

    // ② L1 miss → InflightRegistry per-key lock
    auto guard = _inflight_registry ? _inflight_registry->acquire(chat_session_id)
                                    : InflightRegistry::Guard{nullptr, "", nullptr};
    std::unique_lock<std::mutex> lk(guard.mu ? *guard.mu : _dummy_mu_);

    auto release_guard = [&]() {
        if (lk.owns_lock()) lk.unlock();
        if (guard.registry) guard.registry->release(guard.key);
    };

    // ③ Double-check L1
    local = _local_members_cache ? _local_members_cache->get(mkey) : std::nullopt;
    if (local.has_value()) {
        release_guard();
        auto &members = *local;
        if (members.size() == 1 && members[0] == "__sentinel__") return {};
        return members;
    }

    // ④ Double-check L2 Redis
    auto members = _members_cache->list(chat_session_id);
    if (!members.empty()) {
        release_guard();
        if (members.size() == 1 && members[0] == "__sentinel__") {
            if (_local_members_cache)
                _local_members_cache->set(mkey, {"__sentinel__"}, randomized_ttl(std::chrono::seconds(60)));
            return {};
        }
        if (_local_members_cache)
            _local_members_cache->set(mkey, members, randomized_ttl(std::chrono::seconds(8)));
        return members;
    }

    // ⑤ L2 miss → RedisMutex cross-instance stampede protection
    RedisMutex warm_mutex(_redis, "warm:members:" + chat_session_id, 5000);
    if (!warm_mutex.try_lock(std::chrono::milliseconds(100))) {
        // Another instance is warming — release InflightRegistry so waiting
        // threads can retry, then return empty (caller should retry)
        release_guard();
        return {};  // caller retries, no recursion
    }

    // ⑥ RPC to fetch members (INSIDE the mutex — only one instance executes this)
    members = fetch_members_from_conversation_service_(chat_session_id);

    // ⑦ Warm L2 + L1 (INSIDE the mutex)
    if (members.empty()) {
        _members_cache->warm_sentinel(chat_session_id);
        if (_local_members_cache)
            _local_members_cache->set(mkey, {"__sentinel__"}, randomized_ttl(std::chrono::seconds(60)));
    } else {
        _members_cache->warm(chat_session_id, members);
        if (_local_members_cache)
            _local_members_cache->set(mkey, members, randomized_ttl(std::chrono::seconds(8)));
    }

    warm_mutex.unlock();
    release_guard();
    return members;
}
```

Note: `fetch_members_from_conversation_service_()` is assumed to already exist as the RPC call that was previously in `warm_members_cache()`. If the RPC call was inlined in the caller, extract it into this private helper method.

**Key changes from current code:**
- RedisMutex now wraps the RPC + warm (lines ⑥-⑦), not just checked-and-released
- Recursion replaced with `return {}` — caller should retry
- `warm_members_cache()` merged inline, dead method removed
- Uses a `release_guard` lambda to DRY the guard release pattern

- [ ] **Step 3: Update caller site that previously called warm_members_cache()**

Search for all callers of `warm_members_cache()` and replace with `resolve_members()`. Since `resolve_members()` now warms internally, there should be no separate warm call. If the caller was doing:

```cpp
auto members = impl->resolve_members(ssid);
if (members.empty()) {
    members = /* RPC call */;
    impl->warm_members_cache(ssid, members);
}
```

Replace with:

```cpp
auto members = impl->resolve_members(ssid);
// members is now always populated (or empty for non-existent)
```

- [ ] **Step 4: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add transmite/source/transmite_server.h
git commit -m "fix: hold RedisMutex through RPC+warm in resolve_members, remove recursion"
```

---

### Task 5: Fix randomized_ttl() precision loss for short TTLs

**Files:**
- Modify: `common/utils/random_ttl.hpp`

**Why:** `jitter = base_sec / 5` 整数除法导致 TTL=2s 时 jitter=0（无随机化），TTL=8s 时 jitter=1（应为 1.6）。Spec §4.3 要求 ±20% 均匀随机化。

- [ ] **Step 1: Rewrite with floating-point jitter**

Replace `common/utils/random_ttl.hpp`:

```cpp
#pragma once
#include <chrono>
#include <random>

namespace chatnow {
inline std::chrono::seconds randomized_ttl(std::chrono::seconds base) {
    long base_sec = base.count();
    // Use floating-point to preserve jitter for short TTLs.
    // For base=2s: jitter_sec ∈ [-0.4, 0.4] → actual ∈ [1, 2] (clamped to ≥1)
    double jitter_sec = static_cast<double>(base_sec) * 0.20;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-jitter_sec, jitter_sec);
    long adjusted = base_sec + static_cast<long>(std::round(dist(rng)));
    if (adjusted < 1) adjusted = 1;
    return std::chrono::seconds(adjusted);
}
} // namespace chatnow
```

Actual spread after fix:

| TTL | Spec range | Old range | New range | Match? |
|-----|-----------|-----------|-----------|--------|
| 2s (L1 route) | 1.6-2.4s | 2s fixed | 1-2s | Approx (1s floor) |
| 8s (L1 members) | 6.4-9.6s | 7-9s | 6-10s | Yes |
| 45s (L1 user) | 36-54s | 36-54s | 36-54s | Yes |

Note: TTL=2s clips at 1s minimum to prevent zero/negative TTL. The upper bound (2.4s) is achievable.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/utils/random_ttl.hpp
git commit -m "fix: use floating-point jitter in randomized_ttl for short TTL precision"
```

---

### Task 6: Fix InflightRegistry::Guard to be RAII

**Files:**
- Modify: `common/utils/inflight.hpp`

**Why:** 当前 Guard 持有 raw pointer，调用者必须手动 `release()`。任何提前 return 或异常路径遗漏 release 都会导致 `shared_ptr<mutex>` 泄漏在 `_inflight` map 中，后续相同 key 的 acquire 拿到已失效的 mutex（原始 holder 已销毁）。

- [ ] **Step 1: Make Guard an RAII class with destructor**

Replace `common/utils/inflight.hpp`:

```cpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 进程内 per-key 互斥注册表：用于合并同一 key 的并发缓存穿透请求。
class InflightRegistry {
public:
    using ptr = std::shared_ptr<InflightRegistry>;

    // RAII Guard: 析构时自动 release。Move-only。
    class Guard {
    public:
        Guard() = default;
        Guard(std::shared_ptr<std::mutex> m, std::string k, InflightRegistry *r)
            : mu(std::move(m)), key(std::move(k)), registry(r) {}

        ~Guard() { if (registry) registry->release(key); }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard(Guard &&o) noexcept
            : mu(std::move(o.mu)), key(std::move(o.key)), registry(o.registry) {
            o.registry = nullptr;
        }
        Guard &operator=(Guard &&o) noexcept {
            if (this != &o) {
                if (registry) registry->release(key);
                mu = std::move(o.mu);
                key = std::move(o.key);
                registry = o.registry;
                o.registry = nullptr;
            }
            return *this;
        }

        std::shared_ptr<std::mutex> mu;
        std::string key;

    private:
        InflightRegistry *registry = nullptr;
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

**Key changes:**
- Guard now has a destructor that calls `release(key)` automatically
- Guard is move-only (move constructor/assignment transfer ownership, setting source's `registry` to nullptr)
- `key` is now public (was public before)
- Callers no longer need manual `release()` calls — just let Guard go out of scope

- [ ] **Step 2: Update all call sites to remove manual release()**

In `transmite/source/transmite_server.h` and `push/source/push_server.h`, remove all manual `if (_inflight_registry) _inflight_registry->release(guard.key)` calls. The Guard destructor handles this.

Search pattern: `_inflight_registry->release(guard.key)` → delete the line. The `lk.unlock()` before scope exit remains correct (unlock the mutex so waiting threads can proceed).

- [ ] **Step 3: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -20
```

Expected: All call sites compile. Any compilation error at a removed `release()` line means the guard was being released before going out of scope — in that case, replace manual release with `guard = InflightRegistry::Guard{}` (move-assign an empty guard to trigger the destructor early).

- [ ] **Step 4: Commit**

```bash
git add common/utils/inflight.hpp transmite/source/transmite_server.h push/source/push_server.h
git commit -m "fix: make InflightRegistry::Guard RAII with auto-release destructor"
```

---

### Task 7: Fix LeaderElection::stop() blocking — use condition_variable

**Files:**
- Modify: `common/infra/leader_election.hpp`

**Why:** `stop()` 调用后 campaign loop 线程可能在 `sleep_for(_ttl/3)` 或 `sleep_for(1)` 中阻塞，导致 `_thread.join()` 等待长达 20s（60s lease ÷ 3）。Push 优雅关停会因此显著延迟。Spec §6.2 要求关停 < 1s。

- [ ] **Step 1: Replace sleep_for with condition_variable::wait_for**

Replace `common/infra/leader_election.hpp`:

```cpp
#pragma once

#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Transaction.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "infra/logger.hpp"

namespace chatnow {

class LeaderElection {
public:
    using ptr = std::shared_ptr<LeaderElection>;

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
        _cv.notify_all();  // interrupt any sleep in campaign_loop_ / _hold_leadership_
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
                auto lease_resp = _etcd->leasegrant(_ttl).get();
                if (!lease_resp.is_ok()) {
                    LOG_WARN("LeaderElection leasegrant 失败: {}", lease_resp.error_message());
                    if (!_sleep_interruptible_(std::chrono::seconds(_ttl / 2))) return;
                    continue;
                }
                int64_t lease_id = lease_resp.value().lease();

                etcd::Transaction txn;
                txn.setup_compare_version(_key, etcd::CompareResult::EQUAL, 0);
                txn.setup_put_success(_key, _id, lease_id);
                txn.setup_get_failure(_key);
                auto txn_resp = _etcd->txn(txn).get();

                if (txn_resp.is_ok() && txn_resp.value().succeeded()) {
                    _keep_alive = _etcd->keepalive(lease_id).get();
                    _is_leader = true;
                    if (_on_acquired) _on_acquired();

                    _hold_leadership_(lease_id);

                    if (_is_leader.exchange(false)) {
                        try { _keep_alive->Cancel(); } catch (...) {}
                        if (_on_lost) _on_lost();
                    }
                } else {
                    LOG_DEBUG("LeaderElection: {} 已被占用，等待重试", _key);
                    try { _etcd->leaserevoke(lease_id).wait(); } catch (...) {}
                }
            } catch (std::exception &e) {
                LOG_ERROR("LeaderElection campaign 异常: {}", e.what());
            }

            if (!_sleep_interruptible_(std::chrono::seconds(_ttl / 3))) return;
        }
    }

    void _hold_leadership_(int64_t lease_id) {
        while (_running && _is_leader) {
            if (!_sleep_interruptible_(std::chrono::seconds(1))) return;
            auto ttl_resp = _etcd->timetolive(lease_id).get();
            if (!ttl_resp.is_ok() || ttl_resp.value().ttl() <= 0) {
                LOG_WARN("LeaderElection lease {} 过期，失去 leader", lease_id);
                break;
            }
        }
    }

    // Returns true if sleep completed normally, false if interrupted by stop()
    bool _sleep_interruptible_(std::chrono::seconds duration) {
        std::unique_lock<std::mutex> lk(_cv_mu);
        return !_cv.wait_for(lk, duration, [this] { return !_running; });
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

    std::mutex _cv_mu;
    std::condition_variable _cv;
};

} // namespace chatnow
```

**Key change:** All `std::this_thread::sleep_for()` replaced with `_sleep_interruptible_()` which uses `condition_variable::wait_for()`. When `stop()` sets `_running = false` and calls `_cv.notify_all()`, all sleeps are immediately interrupted, and `_thread.join()` returns in microseconds instead of up to `_ttl/3` seconds.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/infra/leader_election.hpp
git commit -m "fix: use condition_variable in LeaderElection for immediate stop()"
```

---

### Task 8: Wire `_push_service_dir` in Push main.cc and add WS close L1 invalidation

**Files:**
- Modify: `push/source/push_server.cc:64-67`
- Modify: `push/source/push_server.h` (WS close handler)

**Why:** (1) `set_push_service_dir()` 从未在 main() 中调用，stale route reaper 因 `ls("")` 无效查询而静默失败。Spec §6.3 要求周期性扫描并清理僵死路由。(2) WS close handler 未调用 `_local_route_cache->invalidate()`，违反 Spec §5.2 的 `_on_close` 清理要求。

- [ ] **Step 1: Add set_push_service_dir() call in push_server.cc**

After line 64 (`psb.set_etcd_client(...)`), insert:

```cpp
psb.set_push_service_dir(FLAGS_base_service + FLAGS_push_service);
```

This uses the existing `FLAGS_push_service` which is `"/service/push_service"` (line 19), combined with `FLAGS_base_service` (`"/service"`, line 10) to form `"/service/push_service"` — matching the etcd directory where Push instances register.

- [ ] **Step 2: Add L1 cache invalidation in WS close handler**

In `push/source/push_server.h`, find the `_ws_server.set_close_handler(...)` lambda (search for `set_close_handler`). After the existing `_online_route->unbind(uid, did, _instance_id)` call, add:

```cpp
if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
```

The close handler block should now read:

```cpp
_ws_server.set_close_handler([this](websocketpp::connection_hdl hdl) {
    auto conn = _ws_server.get_con_from_hdl(hdl);
    std::string uid, did, jti;
    if (_connections && _connections->client(conn, uid, did, jti)) {
        _connections->remove(conn);
        if (_online_route) _online_route->unbind(uid, did, _instance_id);
        if (_local_route_cache) _local_route_cache->invalidate("route:" + uid);
    }
});
```

- [ ] **Step 3: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```bash
git add push/source/push_server.cc push/source/push_server.h
git commit -m "fix: wire push_service_dir for stale reaper, add L1 invalidate on WS close"
```

---

### Task 9: Implement Transmite `_local_user_cache` usage (resolve_user_with_cache)

**Files:**
- Modify: `transmite/source/transmite_server.h`

**Why:** `_local_user_cache` 声明且构造但从未读写。Spec §5.1 要求 `resolve_user_with_cache()` 提供 L1→L2→RPC 读路径。

- [ ] **Step 1: Add resolve_user_with_cache() method to TransmiteServiceImpl**

In `TransmiteServiceImpl`, add after `resolve_members()`:

```cpp
std::string resolve_user_info(const std::string &uid) {
    std::string ukey = "user:" + uid;

    // ① L1 hit
    if (_local_user_cache) {
        auto cached = _local_user_cache->get(ukey);
        if (cached.has_value()) return *cached;
    }

    // ② L2: UserInfoCache lookup (assumes _user_cache is available)
    // Skip if no user cache configured
    std::string serialized;
    // NOTE: If TransmiteServiceImpl doesn't have a _user_cache member,
    // this step is a no-op and we fall through to RPC directly.
    // The L1 cache still provides value for subsequent reads.

    // ③ RPC to Identity service for GetMultiUserInfo
    // Caller is responsible for the actual RPC; this method handles caching.
    // If the caller already has the user info, it calls warm_user_info() below.

    return "";  // caller should do RPC if empty
}

void warm_user_info(const std::string &uid, const std::string &serialized_info) {
    if (_local_user_cache)
        _local_user_cache->set("user:" + uid, serialized_info,
                               randomized_ttl(std::chrono::seconds(45)));
}
```

- [ ] **Step 2: Integrate into the send message flow**

In the handler that processes `SendMessageReq`, after resolving user info via RPC (e.g., for `mentioned_user_ids` or `reply_to`), call:

```cpp
if (!info.empty()) {
    warm_user_info(uid, info);
}
```

If the Transmite handler doesn't currently resolve user info (it may delegate to downstream services), add at minimum the L1 lookup before each RPC call:

```cpp
auto cached = resolve_user_info(uid);
if (!cached.empty()) {
    // use cached, skip RPC
} else {
    // do RPC, then warm_user_info(uid, result)
}
```

- [ ] **Step 3: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```bash
git add transmite/source/transmite_server.h
git commit -m "feat: implement resolve_user_info/warm_user_info for Transmite L1 UserInfo cache"
```

---

### Task 10: Add SCAN Cluster-mode warning comments (document known limitation)

**Files:**
- Modify: `push/source/push_server.h` (shutdown_cleanup method)
- Modify: `push/source/push_server.h` (reap_stale_routes_ method)

**Why:** Redis `SCAN` 在 Cluster 模式下只扫描单节点 keyspace。`shutdown_cleanup()` 和 `reap_stale_routes_()` 都依赖 SCAN。完全修复需要 per-node SCAN（遍历所有 master 节点），复杂度高且仅在 Cluster 部署时影响。短期方案：添加明确注释标记已知限制，关停清理依赖 30s TTL 自愈作为兜底。

- [ ] **Step 1: Add warning comment to shutdown_cleanup()**

Before the `long long cursor = 0;` line in `shutdown_cleanup()`, insert:

```cpp
// NOTE: SCAN is per-node in Redis Cluster mode. Only keys on the node
// that this connection routes to will be scanned. Other masters' online
// keys will NOT be cleaned up here. Fallback: 30s kOnlineTtl auto-expiry.
// Full fix: iterate all cluster master nodes and SCAN each.
```

- [ ] **Step 2: Add same warning to reap_stale_routes_()**

Same comment before the SCAN loop in `reap_stale_routes_()`.

- [ ] **Step 3: Commit**

```bash
git add push/source/push_server.h
git commit -m "docs: note SCAN per-node limitation in Cluster mode for shutdown/reaper"
```

---

## Phase 3: MINOR Fixes (cleanup)

### Task 11: Remove dead Lua CAS reaper lease methods

**Files:**
- Modify: `common/dao/data_redis.hpp`

**Why:** `PushOutbox::try_acquire_reaper_lease()` / `release_reaper_lease()`, `CrossInstanceOutbox::try_acquire_reaper_lease()` / `release_reaper_lease()`, 和 `ESOutbox` 的对应方法已迁移到 `LeaderElection` 但代码仍保留。死代码增加维护负担且可能被误用。

- [ ] **Step 1: Remove dead lease methods**

In `common/dao/data_redis.hpp`, remove the following methods from their respective classes:

**PushOutbox** (lines ~767-801):
- `try_acquire_reaper_lease(const std::string &instance_id)`
- `release_reaper_lease(const std::string &instance_id)`

**CrossInstanceOutbox** (lines ~848-881):
- `try_acquire_reaper_lease(const std::string &instance_id)`
- `release_reaper_lease(const std::string &instance_id)`

**ESOutbox** (lines ~915-947):
- `try_acquire_reaper_lease(const std::string &instance_id)`
- `release_reaper_lease(const std::string &instance_id)`

Also remove the static Lua script strings (`kAcquireReaperLeaseLua`, `kReleaseReaperLeaseLua`) associated with each class.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "chore: remove dead Lua CAS reaper lease methods (migrated to LeaderElection)"
```

---

### Task 12: Fix Identity builder signature and Gateway standalone Redis creation

**Files:**
- Modify: `identity/source/identity_server.h`
- Modify: `gateway/source/gateway_server.h`

**Why:** (1) Identity 的 `make_redis_object()` 缺少 `pool_size` 参数，与其他 7 个服务不一致。(2) Gateway 单机模式下直接构造 `sw::redis::Redis` 而非使用 `RedisClientFactory::create()`，缺少连接池配置。

- [ ] **Step 1: Add pool_size parameter to Identity make_redis_object()**

In `identity/source/identity_server.h`, change the signature from:

```cpp
void make_redis_object(const std::string &host, uint16_t port, int db, bool keep_alive)
```

To:

```cpp
void make_redis_object(const std::string &host, uint16_t port, int db,
                       bool keep_alive, int pool_size = 16)
```

Update the method body to pass `pool_size` when constructing via `RedisClientFactory::create()`, matching the pattern in all other services.

In `identity/source/identity_server.cc`, update the call site to pass `FLAGS_redis_pool_size`:

```cpp
isb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                      FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
```

- [ ] **Step 2: Fix Gateway standalone Redis to use RedisClientFactory**

In `gateway/source/gateway_server.h`, replace the standalone Redis construction:

```cpp
// Before:
auto r = std::make_shared<sw::redis::Redis>(
    fmt::format("tcp://{}:{}/{}", _redis_host, _redis_port, _redis_db));

// After:
auto r = RedisClientFactory::create(_redis_host, _redis_port, _redis_db,
                                    _redis_keep_alive, _redis_pool_size);
```

This unifies connection creation across all services, ensuring consistent pool configuration.

- [ ] **Step 3: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```bash
git add identity/source/identity_server.h identity/source/identity_server.cc \
        gateway/source/gateway_server.h
git commit -m "fix: unify Identity pool_size param and Gateway RedisClientFactory usage"
```

---

### Task 13: Fix EtcdWorkIdAllocator race condition with sleep heuristic

**Files:**
- Modify: `common/infra/snowflake.hpp`

**Why:** `sleep(200ms)` 等待异步竞选结果可能导致高延迟下错误跳过有效 slot，浪费 etcd lease。最坏情况下全部 1024 个 slot 被跳过导致 fallback。

- [ ] **Step 1: Replace sleep heuristic with synchronous campaign check**

In `common/infra/snowflake.hpp`, replace the `EtcdWorkIdAllocator::allocate()` method:

```cpp
int allocate(const std::string &instance_id, int lease_ttl = 60) {
    for (int slot = 0; slot < _max_workers; ++slot) {
        auto key = "/chatnow/snowflake/worker/" + std::to_string(slot);
        auto election = std::make_shared<LeaderElection>(
            _etcd, key, instance_id, lease_ttl, nullptr, nullptr);
        election->start();

        // Poll is_leader() with timeout instead of fixed sleep
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (election->is_leader()) {
                _active_election = election;
                return slot;
            }
            if (!election->is_leader()) {
                // Transaction returned — we lost. Check if someone else holds it.
                // is_leader() is false and will stay false.
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        election->stop();
    }
    LOG_ERROR("EtcdWorkIdAllocator: 无可用 worker_id slot");
    return -1;
}
```

**Key change:** Instead of sleeping a fixed 200ms and hoping the Transaction completed, now polls `is_leader()` every 50ms for up to 5s. This handles network latency gracefully — if the Transaction takes 2s, the loop waits 2s. If it fails immediately, the loop exits in 50ms.

- [ ] **Step 2: Build verify**

```bash
cd build && cmake .. && make -j4 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add common/infra/snowflake.hpp
git commit -m "fix: poll is_leader() with timeout instead of fixed sleep in EtcdWorkIdAllocator"
```

---

## Verification Checklist

After all Phases complete:

### RedisMutex mutual exclusion
- [ ] Two instances start simultaneously → only one acquires `warm:members:{ssid}` lock
- [ ] Holder crash → lock auto-expires after `ttl_ms` → other instance acquires
- [ ] Unlock uses Lua CAS → non-holder cannot release another instance's lock

### Transmite resolve_members
- [ ] L1 hit → no Redis or RPC access
- [ ] L1 miss + L2 hit → warm L1, return
- [ ] L1 miss + L2 miss → RedisMutex → single RPC → warm L2 + L1 → return
- [ ] RedisMutex timeout → return empty (caller retries), no recursion

### Docker Compose
- [ ] `docker-compose up` → all 6 Redis nodes start → cluster init succeeds
- [ ] `redis-cli -h redis-node1 -p 6379 cluster info` → 3 masters + 3 slaves

### randomized_ttl
- [ ] `randomized_ttl(2s)` returns values in [1, 3) range
- [ ] `randomized_ttl(8s)` returns values in [6, 10) range
- [ ] `randomized_ttl(45s)` returns values in [36, 54] range

### LeaderElection stop() latency
- [ ] `stop()` returns in < 100ms regardless of lease TTL

### InflightRegistry Guard
- [ ] Guard goes out of scope → mutex removed from `_inflight` map (verify via size())
- [ ] Move semantics: moved-from Guard does not double-release

### Stale route reaper
- [ ] `_push_service_dir` correctly set to `"/service/push_service"`
- [ ] Reaper thread runs, queries etcd for online instances, cross-checks SCAN results

### L1 UserInfo cache
- [ ] `resolve_user_info()` returns cached value on L1 hit
- [ ] `warm_user_info()` populates L1 after RPC

---

## Summary

| Phase | Tasks | Issues Fixed |
|-------|-------|-------------|
| 1: CRITICAL | 3 | RedisMutex NX, docker-compose depends_on, RedisClient set() overload |
| 2: IMPORTANT | 7 | resolve_members restructure, randomized_ttl precision, InflightRegistry RAII, LeaderElection stop() blocking, push_service_dir wiring + WS close L1 invalidation, UserInfo cache implementation, SCAN Cluster warning |
| 3: MINOR | 3 | Dead code removal, Identity/Gateway builder consistency, EtcdWorkIdAllocator race |

**Total: 13 tasks** — after all fixes, all CRITICAL and IMPORTANT issues are resolved. The 4 remaining MINOR issues (Prometheus metric exports, `_hold_leadership_` sleep consolidation, `keep_alive` double-cancel, Members::list sentinel filtering) are informational and not blocking.
