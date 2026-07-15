# Cache Resilience Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve open cache issues #35, #37, #38, #45 and the cache/rate-limit portion of #48 with fast Redis failure isolation, bounded local degradation, UserInfo L2 caching, complete TTL jitter, and contention-safe lock retry.

**Architecture:** A per-`RedisClient` lock-free circuit breaker guards all Redis commands; domain DAOs retain responsibility for semantic fallback. `RateLimiter` falls back to a 64-shard in-process token bucket, while UserInfo follows L1 → L2 → singleflight Identity RPC cache-aside. Tests use the PR #49/#54 pure-Go `func`, `reliability`, and `perf` layers.

**Tech Stack:** C++17, redis-plus-plus, bvar, brpc/protobuf, Go 1.23 `testing` + `testify`, Docker Compose, Redis Cluster 7.2.

## Global Constraints

- Production baseline is exactly `origin/3.0-dev@1cad99a`; do not merge the unmerged PR #49 branch into this branch.
- Peak target is 5000 msg/s with 2–8 service instances and groups up to 2000 members.
- Once open, Redis circuit rejection must complete within 10–50ms; no request may retain the old 2s Redis wait.
- Redis outage must retain bounded rate limiting; Redis-backed truth sources must return unavailable instead of fabricated state.
- UserInfo hot-path Identity RPC calls must fall by at least 95%.
- All new tests are Go black-box tests with `func`, `reliability`, or `perf` tags; add no C++ gtest or Python contract tests.
- Do not add an external circuit-breaker service, background probe thread, service-discovery quota splitter, or new runtime dependency.
- UserInfo L2 keys use 64 virtual cluster hash buckets: `im:user:{bucket}:uid`, with `bucket = fnv1a(uid) % 64`.

---

## File Structure

**Create:**

- `common/utils/redis_circuit_breaker.hpp` — atomic Closed/Open/HalfOpen state machine.
- `common/utils/local_rate_limiter.hpp` — bounded 64-shard fallback token bucket.
- `tests/pkg/chaos/redis.go` — stop/start/wait helpers for the six Redis nodes.
- `tests/pkg/verify/redis.go` — Redis CLI and bvar read helpers.
- `tests/func/cache_test.go` — FN-CA-01 through FN-CA-05.
- `tests/reliability/redis_failover_test.go` — RL-05.
- `tests/reliability/setup_test.go` — reliability-tag HTTP setup compatible with PR #49.
- `tests/perf/cache_test.go` — PF-09.

**Modify:**

- `common/dao/data_redis.hpp` — guarded RedisClient commands, UserInfoCache, jittered TTLs, RateLimiter fallback.
- `common/utils/redis_keys.hpp` — UserInfo bucket/key helpers.
- `common/utils/redis_mutex.hpp` — bounded exponential backoff with jitter.
- `common/infra/metrics.hpp` — circuit, fallback, UserInfo, and mutex metrics.
- `transmite/source/transmite_server.h` — UserInfo cache-aside path and dependency injection.
- `identity/source/identity_server.h` — invalidate UserInfo L2 after profile update.
- `tests/Makefile` — reliability target when absent; preserve PR #49-compatible target names.

---

### Task 1: Go Redis chaos and verification helpers

**Files:**

- Create: `tests/pkg/chaos/redis.go`
- Create: `tests/pkg/verify/redis.go`
- Modify: `tests/pkg/client/config.go`
- Modify: `tests/config.yaml`

**Interfaces:**

- Produces: `chaos.StopRedisCluster(testing.TB)`, `chaos.StartRedisCluster(testing.TB)`, `chaos.WaitRedisCluster(testing.TB, time.Duration)`.
- Produces: `verify.RedisCLI(testing.TB, ...string) string`, `verify.RedisTTL(testing.TB, string) time.Duration`, `verify.BVar(testing.TB, string, string) int64`.

- [ ] **Step 1: Add Redis and service metrics endpoints to test config**

Add stable, environment-overridable fields to `tests/pkg/client/config.go` and values to `tests/config.yaml`:

```go
type InfraConfig struct {
	RedisContainer string `yaml:"redis_container"`
	ComposeDir     string `yaml:"compose_dir"`
	TransmiteVars  string `yaml:"transmite_vars"`
}

// Config gains:
Infra InfraConfig `yaml:"infra"`
```

```yaml
infra:
  redis_container: "redis-node1"
  compose_dir: ".."
  transmite_vars: "http://localhost:10004"
```

- [ ] **Step 2: Implement Docker Compose Redis control**

Create `tests/pkg/chaos/redis.go` with these exact service names and recovery check:

```go
package chaos

import (
	"fmt"
	"os/exec"
	"testing"
	"time"
)

var redisServices = []string{
	"redis-node1", "redis-node2", "redis-node3",
	"redis-node4", "redis-node5", "redis-node6",
}

func compose(t testing.TB, args ...string) {
	t.Helper()
	cmd := exec.Command("docker", append([]string{"compose"}, args...)...)
	cmd.Dir = ".."
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("docker compose %v: %v: %s", args, err, out)
	}
}

func StopRedisCluster(t testing.TB) { compose(t, append([]string{"stop"}, redisServices...)...) }
func StartRedisCluster(t testing.TB) { compose(t, append([]string{"start"}, redisServices...)...) }

func WaitRedisCluster(t testing.TB, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		cmd := exec.Command("docker", "exec", "redis-node1", "redis-cli", "cluster", "info")
		if out, err := cmd.Output(); err == nil && string(out) != "" {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("redis cluster did not recover within %s", timeout)
}
```

- [ ] **Step 3: Implement Redis and bvar verification**

Create `tests/pkg/verify/redis.go`:

```go
package verify

import (
	"fmt"
	"io"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"
)

func RedisCLI(t testing.TB, args ...string) string {
	t.Helper()
	base := []string{"exec", "redis-node1", "redis-cli", "-c"}
	out, err := exec.Command("docker", append(base, args...)...).CombinedOutput()
	if err != nil { t.Fatalf("redis-cli %v: %v: %s", args, err, out) }
	return strings.TrimSpace(string(out))
}

func RedisTTL(t testing.TB, key string) time.Duration {
	seconds, err := strconv.ParseInt(RedisCLI(t, "TTL", key), 10, 64)
	if err != nil || seconds < 0 { t.Fatalf("invalid TTL for %s: %d (%v)", key, seconds, err) }
	return time.Duration(seconds) * time.Second
}

func BVar(t testing.TB, baseURL, name string) int64 {
	t.Helper()
	rsp, err := http.Get(fmt.Sprintf("%s/vars/%s", baseURL, name))
	if err != nil { t.Fatalf("read bvar %s: %v", name, err) }
	defer rsp.Body.Close()
	body, err := io.ReadAll(rsp.Body)
	if err != nil { t.Fatalf("read bvar body %s: %v", name, err) }
	value, err := strconv.ParseInt(strings.TrimSpace(string(body)), 10, 64)
	if err != nil { t.Fatalf("parse bvar %s=%q: %v", name, body, err) }
	return value
}
```

- [ ] **Step 4: Compile helper packages**

Run: `cd tests && gofmt -w pkg/chaos/redis.go pkg/verify/redis.go pkg/client/config.go && go test ./pkg/chaos ./pkg/verify ./pkg/client`

Expected: PASS or `[no test files]` for each package, with no compile error.

- [ ] **Step 5: Commit**

```bash
git add tests/pkg/chaos/redis.go tests/pkg/verify/redis.go tests/pkg/client/config.go tests/config.yaml
git commit -m "test: add Redis chaos verification helpers"
```

---

### Task 2: Redis circuit breaker and guarded RedisClient

**Files:**

- Create: `common/utils/redis_circuit_breaker.hpp`
- Modify: `common/dao/data_redis.hpp`
- Modify: `common/infra/metrics.hpp`
- Create: `tests/reliability/setup_test.go`
- Create: `tests/reliability/redis_failover_test.go`
- Modify: `tests/Makefile`

**Interfaces:**

- Produces: `RedisCircuitBreaker::Permit before_call()`, `on_success(Permit)`, `on_connection_failure(Permit)`.
- Produces: `RedisCircuitOpen`, used by semantic fallbacks.
- `RedisClient` public command signatures remain source-compatible.

- [ ] **Step 1: Write failing RL-05 fast-fail/recovery test**

Create `tests/reliability/setup_test.go` with build tag `reliability`, load `tests/config.yaml`, and initialize the package-level HTTP client exactly like `tests/func/setup_test.go`.

Create `tests/reliability/redis_failover_test.go`:

```go
//go:build reliability

package reliability_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// RL-05 | P0 | Redis 熔断后快速失败并自动恢复
func TestRL_RedisCircuitFastFailAndRecovery(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	t.Cleanup(func() { chaos.StartRedisCluster(t); chaos.WaitRedisCluster(t, 60*time.Second) })
	chaos.StopRedisCluster(t)

	uid := user.UserID
	for i := 0; i < 3; i++ {
		rsp := &identity.GetProfileRsp{}
		_ = user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
			RequestId: client.NewRequestID(), UserId: &uid,
		}, rsp)
	}

	started := time.Now()
	rsp := &identity.GetProfileRsp{}
	err := user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(), UserId: &uid,
	}, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	require.Less(t, time.Since(started), 50*time.Millisecond)

	chaos.StartRedisCluster(t)
	chaos.WaitRedisCluster(t, 60*time.Second)
	time.Sleep(1100 * time.Millisecond)
	rsp = &identity.GetProfileRsp{}
	require.NoError(t, user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(), UserId: &uid,
	}, rsp))
	require.True(t, rsp.GetHeader().GetSuccess())
}
```

- [ ] **Step 2: Run RL-05 and verify the old implementation fails**

Run on Linux stack: `cd tests && go test -tags=reliability ./reliability/... -run TestRL_RedisCircuitFastFailAndRecovery -v -count=1 -timeout=180s`

Expected before implementation: FAIL because Redis-backed auth calls retain the old 2s timeout and the final request exceeds 50ms.

- [ ] **Step 3: Implement the atomic circuit state machine**

Create `common/utils/redis_circuit_breaker.hpp` with this public shape and constants:

```cpp
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace chatnow {
class RedisCircuitOpen final : public std::runtime_error {
public: RedisCircuitOpen() : std::runtime_error("redis circuit open") {}
};

class RedisCircuitBreaker {
public:
    enum class State : uint8_t { Closed, Open, HalfOpen };
    struct Permit { bool probe = false; };

    Permit before_call();
    void on_success(Permit permit) noexcept;
    void on_connection_failure(Permit permit) noexcept;
    State state() const noexcept { return _state.load(std::memory_order_acquire); }

private:
    static constexpr uint32_t kFailureThreshold = 3;
    static constexpr int64_t kOpenForMs = 1000;
    static int64_t now_ms() noexcept;
    void open() noexcept;

    std::atomic<State> _state{State::Closed};
    std::atomic<uint32_t> _consecutive_failures{0};
    std::atomic<int64_t> _open_until_ms{0};
};
} // namespace chatnow
```

Use these inline state transitions:

```cpp
inline int64_t RedisCircuitBreaker::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void RedisCircuitBreaker::open() noexcept {
    _open_until_ms.store(now_ms() + kOpenForMs, std::memory_order_release);
    _state.store(State::Open, std::memory_order_release);
}

inline RedisCircuitBreaker::Permit RedisCircuitBreaker::before_call() {
    auto state = _state.load(std::memory_order_acquire);
    if (state == State::Closed) return {};
    if (state == State::HalfOpen) throw RedisCircuitOpen();
    if (now_ms() < _open_until_ms.load(std::memory_order_acquire)) {
        throw RedisCircuitOpen();
    }
    auto expected = State::Open;
    if (_state.compare_exchange_strong(expected, State::HalfOpen,
                                       std::memory_order_acq_rel)) {
        return Permit{true};
    }
    throw RedisCircuitOpen();
}

inline void RedisCircuitBreaker::on_success(Permit) noexcept {
    _consecutive_failures.store(0, std::memory_order_relaxed);
    _state.store(State::Closed, std::memory_order_release);
}

inline void RedisCircuitBreaker::on_connection_failure(Permit permit) noexcept {
    if (permit.probe ||
        _consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1 >=
            kFailureThreshold) {
        open();
    }
}
```

- [ ] **Step 4: Guard every RedisClient command**

In `common/dao/data_redis.hpp`, add `_breaker` and private helpers:

```cpp
template <class F>
decltype(auto) guarded_(F &&fn) {
    auto permit = _breaker.before_call();
    try {
        decltype(auto) result = std::forward<F>(fn)();
        _breaker.on_success(permit);
        return result;
    } catch (const sw::redis::IoError &) {
        _breaker.on_connection_failure(permit); throw;
    } catch (const sw::redis::TimeoutError &) {
        _breaker.on_connection_failure(permit); throw;
    } catch (const sw::redis::ClosedError &) {
        _breaker.on_connection_failure(permit); throw;
    }
}

template <class F>
void guarded_void_(F &&fn) {
    auto permit = _breaker.before_call();
    try { std::forward<F>(fn)(); _breaker.on_success(permit); }
    catch (const sw::redis::IoError &) { _breaker.on_connection_failure(permit); throw; }
    catch (const sw::redis::TimeoutError &) { _breaker.on_connection_failure(permit); throw; }
    catch (const sw::redis::ClosedError &) { _breaker.on_connection_failure(permit); throw; }
}
```

Route `get`, all four `set` overloads, `del`, `expire`, `incr`, both `sadd` overloads, `smembers`, `srem`, `scard`, `hset`, `hget`, `hdel`, `hkeys`, `hgetall`, `hlen`, both `zadd` overloads, `zrem`, `zrange`, `zrangebyscore`, both `eval` overloads, and `scan` through these helpers. Preserve their existing public signatures.

Add a same-slot multi-get used by UserInfo bucket groups:

```cpp
template <typename Input, typename Output>
void mget(Input first, Input last, Output out) {
    guarded_void_([&] {
        _rc ? _rc->mget(first, last, out) : _r->mget(first, last, out);
    });
}
```

Add a move-only `RedisPipeline` wrapper exposing `hset`, `set`, `expire`, `get`, and `exec`; it stores the permit and only reports success/failure when `exec()` runs. Change `RedisClient::pipeline` to return the wrapper.

- [ ] **Step 5: Tighten Redis runtime timeouts and add metrics**

Set standalone and cluster factory values to 50ms connect/socket and 20ms pool wait. Add bvar counters named exactly as the design specifies and increment them only on connection failure, open transition, fast rejection, and recovery.

- [ ] **Step 6: Compile and rerun RL-05**

Run: `cmake -S . -B build && cmake --build build -j2`

Expected: all C++ targets compile.

Run on Linux stack: `cd tests && go test -tags=reliability ./reliability/... -run TestRL_RedisCircuitFastFailAndRecovery -v -count=1 -timeout=180s`

Expected: PASS; stable outage request under 50ms and recovery without service restart.

- [ ] **Step 7: Commit**

```bash
git add common/utils/redis_circuit_breaker.hpp common/dao/data_redis.hpp common/infra/metrics.hpp tests/reliability tests/Makefile
git commit -m "feat: add fast Redis circuit breaking"
```

---

### Task 3: Bounded local rate-limit fallback

**Files:**

- Create: `common/utils/local_rate_limiter.hpp`
- Modify: `common/dao/data_redis.hpp`
- Modify: `common/infra/metrics.hpp`
- Modify: `tests/reliability/redis_failover_test.go`
- Create: `tests/func/cache_test.go`

**Interfaces:**

- Produces: `LocalRateLimiter::allow(key, capacity, window, now)`.
- `RateLimiter::allow_user` and `allow_session` signatures remain unchanged.

- [ ] **Step 1: Add failing Redis-outage rate-limit assertion**

Extend RL-05 after prewarming a conversation. Send 650 unique messages while Redis is stopped and count responses whose header error message is `rate_limited`. Require the count to be greater than zero. Before implementation it is zero because `RateLimiter::allow` returns true on every Redis exception.

Add `FN-CA-05` to `tests/func/cache_test.go`: send 650 messages with Redis healthy and require at least one rate-limited response.

- [ ] **Step 2: Implement sharded token buckets**

Create `common/utils/local_rate_limiter.hpp`:

```cpp
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {
class LocalRateLimiter {
public:
    bool allow(const std::string &key, int capacity, int window_sec);
private:
    struct Bucket { double tokens; int64_t refill_ms; int64_t seen_ms; };
    struct Shard { std::mutex mu; std::unordered_map<std::string, Bucket> buckets; };
    static constexpr size_t kShardCount = 64;
    static constexpr size_t kMaxBuckets = 65536;
    std::array<Shard, kShardCount> _shards;
    std::atomic<size_t> _bucket_count{0};
    std::atomic<uint64_t> _operations{0};
};
} // namespace chatnow
```

Use steady-clock milliseconds. Refill `elapsed * capacity / (window_sec * 1000)`, cap at capacity, consume one token when available, and update `seen_ms`. Every 1024 operations, remove entries idle for more than two windows from the current shard. At the hard cap, deny unknown keys after cleanup.

- [ ] **Step 3: Wire semantic fallback**

Give `RateLimiter` a `LocalRateLimiter _local`. In the existing catch block replace `return true` with:

```cpp
metrics::g_rate_limit_local_fallback_total << 1;
const bool allowed = _local.allow(key_full, max_count, window_sec);
if (!allowed) metrics::g_rate_limit_local_rejected_total << 1;
return allowed;
```

Keep Redis Lua as the only normal path; do not double-write local state.

- [ ] **Step 4: Verify**

Run: `cmake --build build -j2`

Run on Linux stack: `cd tests && go test -tags=func ./func/... -run TestFN_CA_RateLimit -v -count=1 && go test -tags=reliability ./reliability/... -run TestRL_RedisCircuitFastFailAndRecovery -v -count=1 -timeout=180s`

Expected: both healthy Redis and stopped Redis produce bounded rate-limit rejection.

- [ ] **Step 5: Commit**

```bash
git add common/utils/local_rate_limiter.hpp common/dao/data_redis.hpp common/infra/metrics.hpp tests/func/cache_test.go tests/reliability/redis_failover_test.go
git commit -m "feat: retain rate limiting during Redis outages"
```

---

### Task 4: UserInfo L1/L2/RPC cache-aside

**Files:**

- Modify: `common/utils/redis_keys.hpp`
- Modify: `common/dao/data_redis.hpp`
- Modify: `common/infra/metrics.hpp`
- Modify: `transmite/source/transmite_server.h`
- Modify: `identity/source/identity_server.h`
- Modify: `tests/func/cache_test.go`

**Interfaces:**

- Produces: `key::user_info_bucket(uid)`, `key::user_info_key(uid)`.
- Produces: `UserInfoCache::get`, `batch_get`, `set`, `batch_set`, `invalidate` over serialized protobuf strings.
- `TransmiteServiceImpl` gains `UserInfoCache::ptr` but preserves RPC API.

- [ ] **Step 1: Write failing cache behavior tests**

Add these functions to `tests/func/cache_test.go`:

- `TestFN_CA_UserInfoL2AvoidsRepeatedRPC`: record `user_info_rpc_total`, send 20 messages from one user, require delta ≤ 1 and Redis key existence.
- `TestFN_CA_UserInfoSingleflight`: start 200 goroutines sending from one cold user, require `user_info_rpc_total` delta ≤ 1.
- `TestFN_CA_UserInfoInvalidatedAfterProfileUpdate`: warm, update nickname, require old Redis key deleted, send again, then require the stored protobuf reflects the new nickname.

Run on Linux: `cd tests && go test -tags=func ./func/... -run 'TestFN_CA_UserInfo' -v -count=1`

Expected before implementation: FAIL because the L2 key and UserInfo metrics do not exist and every send calls Identity.

- [ ] **Step 2: Add deterministic 64-bucket keys**

In `common/utils/redis_keys.hpp`, add FNV-1a helpers:

```cpp
inline uint32_t fnv1a_32(const std::string &value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) { hash ^= c; hash *= 16777619u; }
    return hash;
}
inline uint32_t user_info_bucket(const std::string &uid) { return fnv1a_32(uid) % 64u; }
inline std::string user_info_key(const std::string &uid) {
    auto bucket = std::to_string(user_info_bucket(uid));
    return "im:user:{" + bucket + "}:" + uid;
}
```

- [ ] **Step 3: Implement UserInfoCache DAO**

In `common/dao/data_redis.hpp`, add `kUserInfoTtl{3600}` and a `UserInfoCache` class that stores strings. `get` catches Redis errors and returns nullopt; `set` uses `randomized_ttl(kUserInfoTtl)`; `invalidate` deletes the key.

`batch_get` groups uids by `user_info_bucket`, caps input at 2000, performs one MGET per bucket, and returns `{hits, misses}` preserving uid association. `batch_set` groups the same way and uses one pipeline per bucket. An empty serialized value is a 5-second sentinel; dependency errors are never cached.

- [ ] **Step 4: Replace Transmite's unconditional profile RPC**

Inject `_user_info_cache` from `TransmiteServerBuilder::make_redis_object`. Replace `resolve_user_info` with:

```cpp
std::optional<std::string> resolve_user_info(const std::string &uid,
                                             const std::string &rid,
                                             brpc::Controller *caller) {
    const auto lkey = key::local_user_info_cache_key(uid);
    if (_local_user_cache) {
        auto local = _local_user_cache->get(lkey);
        if (local) { metrics::g_user_info_l1_hit_total << 1; return local; }
    }
    auto guard = _inflight_registry->acquire("user:" + uid);
    std::unique_lock lk(*guard.mu);
    if (_local_user_cache) if (auto local = _local_user_cache->get(lkey)) return local;
    if (_user_info_cache) if (auto redis = _user_info_cache->get(uid)) {
        metrics::g_user_info_l2_hit_total << 1;
        _local_user_cache->set(lkey, *redis, randomized_ttl(std::chrono::seconds(45)));
        return redis;
    }
    auto info = fetch_user_info_from_identity_(uid, rid, caller);
    if (!info) return std::nullopt;
    auto bytes = info->SerializeAsString();
    if (_user_info_cache) _user_info_cache->set(uid, bytes);
    if (_local_user_cache) _local_user_cache->set(lkey, bytes, randomized_ttl(std::chrono::seconds(45)));
    metrics::g_user_info_rpc_total << 1;
    return bytes;
}
```

Call it before member resolution. Parse cached bytes into `chatnow::common::UserInfo`; corrupted values invalidate L1/L2 and return unavailable only if the subsequent RPC also fails. Remove the old unconditional async GetProfile call.

- [ ] **Step 5: Invalidate after successful profile update**

Construct `UserInfoCache` from Identity's existing `_redis_client`, inject it into `IdentityServiceImpl`, and immediately after `_mysql_user->update(user)` succeeds call `_user_info_cache->invalidate(auth.user_id)`. Cache deletion failure must not roll back the DB update.

- [ ] **Step 6: Verify**

Run: `cmake --build build -j2`

Run on Linux: `cd tests && go test -tags=func ./func/... -run 'TestFN_CA_UserInfo' -v -count=1`

Expected: all three tests PASS; 20 repeated sends cause at most one Identity RPC and profile update removes L2.

- [ ] **Step 7: Commit**

```bash
git add common/utils/redis_keys.hpp common/dao/data_redis.hpp common/infra/metrics.hpp transmite/source/transmite_server.h identity/source/identity_server.h tests/func/cache_test.go
git commit -m "feat: add UserInfo multilevel caching"
```

---

### Task 5: Complete TTL jitter and RedisMutex backoff

**Files:**

- Modify: `common/dao/data_redis.hpp`
- Modify: `common/utils/redis_mutex.hpp`
- Modify: `common/infra/metrics.hpp`
- Modify: `tests/func/cache_test.go`

**Interfaces:**

- Existing DAO method signatures remain unchanged.
- UnackedPush's paired keys receive one shared randomized TTL sample per operation.

- [ ] **Step 1: Write failing FN-CA-04 TTL test**

Use unique users/devices, exercise login/status/code/device/unacked API paths, read TTLs through `verify.RedisTTL`, and require each TTL to lie within `[0.8*base-2s, 1.2*base+2s]`. Collect at least 20 keys and require at least two distinct TTL values.

Run on Linux: `cd tests && go test -tags=func ./func/... -run TestFN_CA_TTLJitter -v -count=1`

Expected before implementation: FAIL because Session/Status/Codes/DeviceSet/UnackedPush use fixed or absent TTLs.

- [ ] **Step 2: Apply jitter to every missing path**

Use `randomized_ttl(ttl)` in Session append/touch, Status append/touch, Codes append, and any remaining fixed cache expiration found by `rg '_c->(set|expire).*ttl' common/dao/data_redis.hpp`.

In `DeviceSet::add`, call `expire(key, randomized_ttl(kSessionTtl))`; add `touch(uid, ttl=kSessionTtl)` and invoke it from device activity paths.

In `UnackedPush::push` and `bump_score`, calculate exactly once:

```cpp
const auto effective_ttl = randomized_ttl(ttl);
_c->expire(k, effective_ttl);
_c->expire(ik, effective_ttl);
```

- [ ] **Step 3: Implement bounded exponential lock retry**

Replace the fixed 5ms sleep in `RedisMutex::try_lock` with:

```cpp
int backoff_ms = 5;
while (std::chrono::steady_clock::now() < deadline) {
    if (_redis->set(_key, _token, std::chrono::milliseconds(_ttl_ms),
                    sw::redis::UpdateType::NOT_EXIST)) {
        _locked = true;
        return true;
    }
    metrics::g_redis_mutex_retry_total << 1;
    std::uniform_int_distribution<int> jitter(0, backoff_ms / 2);
    auto delay = std::chrono::milliseconds(backoff_ms + jitter(rng_()));
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) break;
    std::this_thread::sleep_for(std::min(delay, remaining));
    backoff_ms = std::min(backoff_ms * 2, 20);
}
```

Add a private thread-local RNG accessor; preserve SET NX PX and Lua CAS unlock unchanged.

- [ ] **Step 4: Verify**

Run: `cmake --build build -j2`

Run on Linux: `cd tests && go test -tags=func ./func/... -run TestFN_CA_TTLJitter -v -count=1`

Expected: PASS with in-range, non-identical TTL samples.

- [ ] **Step 5: Commit**

```bash
git add common/dao/data_redis.hpp common/utils/redis_mutex.hpp common/infra/metrics.hpp tests/func/cache_test.go
git commit -m "perf: jitter cache TTLs and lock retries"
```

---

### Task 6: PF-09 performance baseline and complete verification

**Files:**

- Create: `tests/perf/cache_test.go`
- Modify: `tests/Makefile`
- Modify: `docs/superpowers/specs/2026-07-15-cache-resilience-completion-design.md` only if implementation constraints require a documented correction.

**Interfaces:**

- Produces: `BenchmarkPF09_UserInfoCache` with cold/L2/L1 sub-benchmarks and explicit RPC-reduction metric.

- [ ] **Step 1: Write PF-09 benchmark**

Create a perf-tag benchmark that prepares 20 conversations, runs parallel sends from repeated senders, records elapsed latency samples, and calls:

```go
b.ReportMetric(float64(successes)/elapsed.Seconds(), "msg/s")
b.ReportMetric(float64(p95.Microseconds()), "p95-us")
b.ReportMetric(100*(1-float64(rpcDelta)/float64(successes)), "rpc-reduction-%")
```

Fail when RPC reduction is below 95%, steady-state throughput is below 5000 msg/s in the target Linux environment, or the checked-in baseline throughput regresses by more than 10%.

- [ ] **Step 2: Run static and compile verification**

Run:

```bash
git diff --check
cmake -S . -B build
cmake --build build -j2
cd tests
gofmt -w pkg/chaos/redis.go pkg/verify/redis.go func/cache_test.go reliability/*.go perf/cache_test.go
go vet -tags=func ./func/... ./pkg/...
go vet -tags=reliability ./reliability/... ./pkg/...
go vet -tags=perf ./perf/... ./pkg/...
go test -run '^$' ./...
```

Expected: zero formatting errors, all production targets compile, and all Go packages compile.

- [ ] **Step 3: Run Linux full-stack validation**

Run:

```bash
cd tests
make test-func
make test-reliability
go test -tags=perf ./perf/... -run '^$' -bench BenchmarkPF09_UserInfoCache -benchmem -count=3 -benchtime=10s
```

Expected: FN-CA-01..05 and RL-05 PASS; PF-09 reports ≥5000 msg/s, p95, and ≥95% RPC reduction.

- [ ] **Step 4: Review the issue acceptance matrix**

Confirm with evidence:

- #35: RL-05 shows fast circuit rejection and automatic recovery.
- #37: FN-CA-01..03 and PF-09 show L2 behavior and ≥95% RPC reduction.
- #38: FN-CA-04 shows jitter and DeviceSet TTL.
- #45: contention metrics show bounded jittered retry.
- #48 cache/rate-limit scope: RL-05 shows local rejection during Redis outage.

- [ ] **Step 5: Commit**

```bash
git add tests/perf/cache_test.go tests/Makefile docs/superpowers/specs/2026-07-15-cache-resilience-completion-design.md
git commit -m "test: add cache resilience performance baseline"
```

- [ ] **Step 6: Invoke verification-before-completion and requesting-code-review**

Run the complete verification commands again with fresh output, inspect the full diff against `origin/3.0-dev`, and do not claim completion until both reviews find no unresolved correctness, availability, concurrency, or test-framework issue.
