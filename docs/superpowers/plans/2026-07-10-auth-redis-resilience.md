# Auth and Redis Resilience Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close #21, #22, and #48: deny revoked WebSocket tokens, remove committed secrets, and keep Redis outages bounded and safe.

**Architecture:** Add standard-library-only process-local circuit-breaker and bounded striped fallback-bucket primitives. Replace JWT-store boolean results with explicit `present/absent/unavailable` outcomes so sensitive paths fail closed. Redis Lua remains the healthy distributed limiter.

**Tech Stack:** C++17, brpc, gflags, sw::redis++, GoogleTest/assert common tests, Python static contract test.

## Global Constraints

- Work on the current branch containing latest `3.0-dev`; preserve its existing Redis cache edits.
- Breaker state uses atomics and `steady_clock`; open means no Redis I/O and cooldown permits exactly one probe.
- JWT access, refresh/replay and Push admission fail closed when Redis state is unavailable.
- Redis stays authoritative while healthy; degradation is a bounded striped, per-instance bucket that denies once empty.
- Preserve Transmite's database-backed idempotency fail-open, but bypass Redis while the breaker is open.
- MySQL/SMTP secrets have no runnable default; missing runtime values stop startup before client construction.
- Do not add Redis checks to per-message or per-push hot paths; never log credentials, tokens, or JTIs.

---

## File map

- Create `common/infra/redis_resilience.hpp`: reusable circuit breaker and local bucket.
- Modify `common/dao/data_redis.hpp`: timeout defaults, breaker-gated limiter, idempotency gate seam.
- Modify `common/auth/jwt_store.hpp`, Gateway, Identity, and Push: explicit result and fail-closed consumers.
- Modify `identity/source/identity_server.cc` and configs: safe secret defaults and validation.
- Create/modify focused common tests and `common/test/test_data_redis_contract.py`.

### Task 1: Required runtime secrets (#22)

**Files:** `identity/source/identity_server.cc:20-50`, `conf/local/identity_server.conf`, `conf/docker/identity_server.conf`, `common/test/test_data_redis_contract.py`.

**Produces:** `validate_required_secret(const char*, const std::string&)`, invoked before `make_mysql_object` and `make_mail_object`.

- [ ] **Step 1: Write RED contract test**

```python
def test_identity_secrets_have_no_runnable_defaults():
    source = (ROOT / "identity/source/identity_server.cc").read_text()
    assert 'DEFINE_string(mysql_pswd, "",' in source
    assert 'DEFINE_string(mail_paswd, "",' in source
    assert 'YHY060403' not in source and 'XKk5zvYwWKeB8xNk' not in source
    assert 'validate_required_secret("mysql_pswd", FLAGS_mysql_pswd);' in source
    assert 'validate_required_secret("mail_paswd", FLAGS_mail_paswd);' in source
```

- [ ] **Step 2: Verify RED**

Run: `python3 common/test/test_data_redis_contract.py`

Expected: assertion failure, because defaults are populated and validation is absent.

- [ ] **Step 3: Implement minimum startup check**

```cpp
namespace {
void validate_required_secret(const char* name, const std::string& value) {
    if (value.empty()) { LOG_ERROR("required secret is empty: {}", name); std::exit(EXIT_FAILURE); }
}
}
DEFINE_string(mysql_pswd, "", "MySQL password (required secret)");
DEFINE_string(mail_paswd, "", "SMTP password (required secret)");
// Immediately after init_logger(), before any make_* call:
validate_required_secret("mysql_pswd", FLAGS_mysql_pswd);
validate_required_secret("mail_paswd", FLAGS_mail_paswd);
```

Replace config values with non-secret injection placeholders and comments.

- [ ] **Step 4: Verify GREEN**

Run: `python3 common/test/test_data_redis_contract.py && git diff --check`

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add identity/source/identity_server.cc conf/local/identity_server.conf conf/docker/identity_server.conf common/test/test_data_redis_contract.py && git commit -m "fix(identity): require injected database and mail secrets"
```

### Task 2: Testable breaker and fallback token bucket (#48)

**Files:** Create `common/infra/redis_resilience.hpp`, `common/test/test_redis_resilience.cc`; modify `common/test/CMakeLists.txt`.

**Produces:** `RedisCircuitBreaker::allow_request()/record_success()/record_failure()` and `LocalTokenBucket::allow(key, capacity, window_sec)`.

- [ ] **Step 1: Write RED unit tests**

```cpp
TEST(RedisCircuitBreaker, OpensAndAllowsOnlyOneRecoveryProbe) {
    RedisCircuitBreaker b(std::chrono::milliseconds(10));
    ASSERT_TRUE(b.allow_request()); b.record_failure();
    EXPECT_FALSE(b.allow_request());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_TRUE(b.allow_request()); EXPECT_FALSE(b.allow_request());
    b.record_success(); EXPECT_TRUE(b.allow_request());
}
TEST(LocalTokenBucket, ConcurrentCallsNeverExceedCapacity) {
    LocalTokenBucket bucket(8, 32); std::atomic<int> allowed{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 32; ++i) {
        workers.emplace_back([&] { if (bucket.allow("u:1", 5, 60)) ++allowed; });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(allowed.load(), 5);
}
```

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --target test_redis_resilience -j2`

Expected: missing target/type compile failure.

- [ ] **Step 3: Implement minimum primitives**

```cpp
class RedisCircuitBreaker {
public:
 explicit RedisCircuitBreaker(std::chrono::milliseconds cooldown);
 bool allow_request(); void record_success(); void record_failure();
private: std::atomic<State> _state{State::Closed}; std::atomic<int64_t> _retry_after_ns{0};
};
class LocalTokenBucket {
public: LocalTokenBucket(size_t stripes, size_t max_entries);
 bool allow(const std::string&, int capacity, int window_sec);
};
```

Use stripe mutex/map pairs; refill with `steady_clock`, remove expired entries before insertion at per-stripe capacity, and preserve unlimited behavior for invalid limits.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build build --target test_redis_resilience -j2 && ./build/common/test/test_redis_resilience`

Expected: exit 0, including no concurrent over-issue.

- [ ] **Step 5: Commit**

```bash
git add common/infra/redis_resilience.hpp common/test/test_redis_resilience.cc common/test/CMakeLists.txt && git commit -m "feat(common): add Redis breaker and local rate-limit bucket"
```

### Task 3: Bound Redis failure and enforce local rate-limit fallback (#48)

**Files:** `common/dao/data_redis.hpp:1-285,869-950`, `transmite/source/transmite_server.h`, `common/test/test_data_redis_contract.py`, `common/test/test_redis_resilience.cc`.

**Consumes:** Task 2 primitives. **Produces:** breaker-gated `RateLimiter::allow`.

- [ ] **Step 1: Write RED contract test**

```python
def test_limiter_short_circuits_and_degrades_conservatively():
    source = (ROOT / "common/dao/data_redis.hpp").read_text()
    assert '#include "infra/redis_resilience.hpp"' in source
    assert 'if (!_breaker.allow_request())' in source
    assert 'return _fallback.allow(key_full, max_count, window_sec);' in source
    assert 'std::chrono::milliseconds(200)' in source
    assert 'std::chrono::milliseconds(100)' in source
```

- [ ] **Step 2: Verify RED**

Run: `python3 common/test/test_data_redis_contract.py`

Expected: assertion failure; current timeouts are 2000ms/500ms and exception returns `true`.

- [ ] **Step 3: Integrate with unchanged healthy Lua behavior**

```cpp
if (!_breaker.allow_request()) return _fallback.allow(key_full, max_count, window_sec);
try {
    const auto cur = _c->eval<long long>(kRateLimitScript, keys.begin(), keys.end(), args.begin(), args.end());
    _breaker.record_success(); return cur == 1;
} catch (const std::exception& e) {
    _breaker.record_failure(); LOG_ERROR("RateLimiter Redis failure: {}", e.what());
    return _fallback.allow(key_full, max_count, window_sec);
}
```

Give both factories named defaults: connect/socket 200ms and pool wait 100ms. Preserve existing-call compatibility. Gate Transmite's idempotency Redis sequence with the same breaker but preserve its current database continuation.

- [ ] **Step 4: Verify GREEN**

Run: `python3 common/test/test_data_redis_contract.py && cmake --build build --target test_redis_resilience -j2 && ./build/common/test/test_redis_resilience`

Expected: exit 0; no unconditional error-path allow remains.

- [ ] **Step 5: Commit**

```bash
git add common/dao/data_redis.hpp transmite/source/transmite_server.h common/test/test_data_redis_contract.py common/test/test_redis_resilience.cc && git commit -m "fix(rate-limit): bound Redis failures and enforce local fallback"
```

### Task 4: Explicit JWT-store outcomes and fail-closed consumers (#48)

**Files:** `common/auth/jwt_store.hpp:32-155`, `gateway/source/gateway_auth.hpp:49-105`, `identity/source/identity_server.h:226-280`, `common/test/test_jwt_store.cc`, `common/test/CMakeLists.txt`.

**Produces:** `JwtStore::ReadResult { kPresent, kAbsent, kUnavailable }`, `revocation_state(jti)`, and `active_refresh_state(uid, did, std::string*)`.

- [ ] **Step 1: Write RED test**

```cpp
TEST(JwtStore, RevocationStateDistinguishesMissingFromUnavailable) {
    JwtStore healthy(make_redis());
    EXPECT_EQ(healthy.revocation_state("missing"), JwtStore::ReadResult::kAbsent);
    healthy.revoke("revoked", 60);
    EXPECT_EQ(healthy.revocation_state("revoked"), JwtStore::ReadResult::kPresent);
    sw::redis::ConnectionOptions options;
    options.host = "127.0.0.1"; options.port = 1;
    options.connect_timeout = std::chrono::milliseconds(20);
    options.socket_timeout = std::chrono::milliseconds(20);
    JwtStore unreachable_store(std::make_shared<RedisClient>(
        std::make_shared<sw::redis::Redis>(options)));
    EXPECT_EQ(unreachable_store.revocation_state("any"), JwtStore::ReadResult::kUnavailable);
}
```

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --target test_jwt_store -j2 && ./build/common/test/test_jwt_store`

Expected: compile failure because the explicit API does not exist.

- [ ] **Step 3: Implement explicit values and exact caller policy**

```cpp
const auto state = store->revocation_state(claims.jti);
if (state != JwtStore::ReadResult::kAbsent) {
    write_401(::chatnow::error::kAuthTokenInvalid, "auth unavailable or revoked");
    return false;
}
```

In `RefreshToken`, unavailable revocation, active-refresh, rotate, revoke, or clear operation returns `kSystemUnavailable` before a replacement token is returned. Confirmed revoked remains `kAuthTokenInvalid`; confirmed replay/mismatch remains `kAuthRefreshTokenReused`.

- [ ] **Step 4: Verify GREEN**

Run: `cmake --build build --target test_jwt_store -j2 && ./build/common/test/test_jwt_store`

Expected: lifecycle/replay tests and unavailable-state test pass.

- [ ] **Step 5: Commit**

```bash
git add common/auth/jwt_store.hpp gateway/source/gateway_auth.hpp identity/source/identity_server.h common/test/test_jwt_store.cc common/test/CMakeLists.txt && git commit -m "fix(auth): fail closed when Redis token state is unavailable"
```

### Task 5: Check revocation before Push WebSocket side effects (#21)

**Files:** `push/source/push_server.h:56-90,430-475,1130-1180`, `push/source/push_server.cc`, `common/test/test_data_redis_contract.py`.

**Consumes:** Task 4 `JwtStore::revocation_state`. **Produces:** Push-owned non-null `JwtStore` constructed from its Redis client.

- [ ] **Step 1: Write RED admission contract**

```python
def test_push_checks_revocation_before_registration():
    source = (ROOT / "push/source/push_server.h").read_text()
    assert source.index('auto state = _jwt_store->revocation_state(jti);') < source.index('_connections->insert(conn, uid, did, jti);')
    assert 'if (state != auth::JwtStore::ReadResult::kAbsent)' in source
    assert 'std::shared_ptr<chatnow::auth::JwtStore> _jwt_store;' in source
```

- [ ] **Step 2: Verify RED**

Run: `python3 common/test/test_data_redis_contract.py`

Expected: assertion failure; Push only verifies JWT signature today.

- [ ] **Step 3: Inject store and reject before insert/bind/presence**

```cpp
const auto state = _jwt_store->revocation_state(jti);
if (state != auth::JwtStore::ReadResult::kAbsent) {
    LOG_WARN("WS token state rejected uid={}", uid);
    conn->close(websocketpp::close::status::unsupported_data, "auth failed");
    return;
}
// only now insert the connection, bind route, and write presence
```

Build the store in `PushServerBuilder` from `_redis_client` and reject missing store alongside missing Redis/JWT setup. Do not log the JTI.

- [ ] **Step 4: Verify GREEN**

Run: `python3 common/test/test_data_redis_contract.py && cmake --build build --target push_server -j2`

Expected: exit 0; source contract establishes ordering before every side effect.

- [ ] **Step 5: Commit**

```bash
git add push/source/push_server.h push/source/push_server.cc common/test/test_data_redis_contract.py && git commit -m "fix(push): reject revoked tokens during WebSocket auth"
```

### Task 6: Operational rotation instruction and full focused verification

**Files:** `docs/operations/jwt-key-rotation.md`, `common/test/test_data_redis_contract.py`.

- [ ] **Step 1: Write RED documentation contract**

```python
def test_credential_rotation_runbook_exists():
    text = (ROOT / "docs/operations/jwt-key-rotation.md").read_text().lower()
    assert "mysql" in text and "smtp" in text and "rotate" in text
```

- [ ] **Step 2: Verify RED**

Run: `python3 common/test/test_data_redis_contract.py`

Expected: assertion failure until operational note is added.

- [ ] **Step 3: Add only operationally safe guidance**

Document: rotate previously committed MySQL/SMTP credentials before deployment; inject new values through the deployment secret mechanism; deploy Identity replicas; verify missing-secret startup failure; revoke old-secret access. Never include a replacement secret.

- [ ] **Step 4: Full focused verification**

Run: `python3 common/test/test_data_redis_contract.py && cmake --build build --target test_redis_resilience test_jwt_store push_server -j2 && ./build/common/test/test_redis_resilience && ./build/common/test/test_jwt_store && git diff --check`

Expected: every command exits 0.

- [ ] **Step 5: Commit**

```bash
git add docs/operations/jwt-key-rotation.md common/test/test_data_redis_contract.py && git commit -m "docs: add credential rotation runbook"
```

## Plan self-review

- #21: Task 5 uses Task 4 explicit state before connection/presence side effects.
- #22: Task 1 removes defaults and Task 6 requires out-of-repo rotation.
- #48: Tasks 2-4 cover short-circuiting, timeout bounds, local fallback, JWT fail-closed, and idempotency bypass.
- Non-goals remain periodic established-WebSocket checks and cross-instance-exact degraded limiting.
