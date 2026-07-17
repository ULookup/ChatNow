# PR #57 Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve all three requested-change threads with explicit cache fencing, an identity-stable Unacked pending ledger, and executable RL-05/PF-09 CI gates.

**Architecture:** UserInfo cache fills expose committed/conflict/unavailable outcomes and publish L1 only when the outcome permits it. Unacked uses a ZSET of stable `user_seq` members plus a HASH of payloads, maintained atomically by Lua. Reliability and cache performance run as separate full-stack workflow jobs.

**Tech Stack:** C++17, redis-plus-plus, Redis Cluster Lua, Go 1.24 tests, Docker Compose, GitHub Actions.

## Global Constraints

- Treat `seq_id`, `user_seq`, and `message_id` as distinct domains.
- Do not implement issue #58 in this change.
- Do not read or migrate the former `user_seq:payload` ZSET layout.
- Use the new Go test framework and preserve default production rate limits.
- Follow red-green-refactor and commit each task independently.

---

### Task 1: Make UserInfo generation fencing explicit

**Files:**
- Modify: `common/dao/data_redis.hpp`
- Modify: `transmite/source/transmite_server.h`
- Create: `common/test/test_user_info_generation_fence.cc`

**Interfaces:**
- Produce: `enum class GenerationWriteResult { Committed, Conflict, Unavailable }`.
- Produce: `UserInfoCache::set_if_generation(...) -> GenerationWriteResult`.
- Preserve: `batch_set(...) -> size_t`, counting only committed writes.

- [ ] **Step 1: Write the failing policy test**

Create a focused test that asserts `Committed` permits L1 publication,
`Conflict` denies it, and `Unavailable` permits only the short-lived fallback.
It must also assert that a generation-aware fill cannot treat `Conflict` as
Redis unavailability.

- [ ] **Step 2: Run the test and verify RED**

Run the repository's existing C++ test compile command for
`test_user_info_generation_fence.cc`. Expected: compilation fails because
`GenerationWriteResult` and the publication policy do not exist.

- [ ] **Step 3: Implement the three-state result**

Return `Conflict` only when Lua executes and returns zero. Return `Unavailable`
for a missing Redis client, `RedisCircuitOpen`, or another Redis exception.
Update batch writes to count only `Committed` entries.

- [ ] **Step 4: Enforce the policy in Transmite**

Store the CAS result. Write L1 after `Committed`; skip L1 after `Conflict`;
write the existing randomized 45-second L1 fallback only when generation could
not be observed or the fenced write returns `Unavailable`. Always return the
Identity response to the current caller.

- [ ] **Step 5: Verify GREEN and regressions**

Run the new test, the existing cache harness, C++ syntax compilation for
`transmite_server.h`, and `git diff --check`. Expected: all exit zero.

- [ ] **Step 6: Commit**

Commit message: `fix(cache): fence UserInfo L1 publication`.

### Task 2: Redesign Unacked as a stable pending ledger

**Files:**
- Modify: `common/dao/data_redis.hpp`
- Create: `common/test/test_unacked_pending_ledger.cc`
- Modify: `tests/func/cache_test.go` only if the new framework needs black-box coverage.

**Interfaces:**
- Preserve: `push`, `ack`, `peek_due`, and `bump_score` C++ signatures.
- Change Redis ZSET member to decimal `user_seq` only.
- Keep HASH field as decimal `user_seq` and value as `payload_b64`.

- [ ] **Step 1: Write the failing real-Redis tests**

Cover: pushing the same sequence with payload A then B leaves `ZCARD == 1` and
returns B; ACK removes both keys' entries; a ZSET-only orphan and a HASH-only
orphan are removed by due-read; bump updates only complete entries. Use the
existing Redis Cluster test setup and unique uid/device keys.

- [ ] **Step 2: Run the tests and verify RED**

Expected failures: duplicate ZSET members for changed payload, payload parsing
from the member string, and orphan entries remaining.

- [ ] **Step 3: Replace push and ACK scripts**

Push atomically executes `ZADD key score user_seq`, `HSET index user_seq payload`,
and paired expiry. ACK atomically executes `ZREM key user_seq` and
`HDEL index user_seq` without depending on payload contents.

- [ ] **Step 4: Make due-read and bump atomic and self-healing**

Implement due-read as Lua returning a flat sequence/payload array. For every due
sequence, return it only if the HASH payload exists; otherwise remove the ZSET
member. Remove HASH-only entries encountered by the bounded consistency pass.
Bump scores only when the HASH contains the sequence; otherwise remove the ZSET
member. Renew the paired TTL sample in mutating scripts.

- [ ] **Step 5: Verify GREEN and regressions**

Run the new real-Redis suite against the six-node cluster, the existing
DeviceSet/OnlineRoute/Unacked cluster suite, RedisMutex syntax validation, and
`git diff --check`. Expected: all exit zero and no orphan remains.

- [ ] **Step 6: Commit**

Commit message: `fix(push): make unacked retries identity-idempotent`.

### Task 3: Wire RL-05 and PF-09 into CI

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `docker-compose.yml`
- Modify: `tests/Makefile`
- Create: `tests/pkg/contracts/ci_gates_test.go`

**Interfaces:**
- Produce workflow job `reliability` invoking `make test-reliability`.
- Produce scheduled workflow job `perf-cache` invoking `make test-perf-cache-gate`.
- Consume Compose variables `TRANSMITE_RATE_LIMIT_USER_MAX` and
  `TRANSMITE_RATE_LIMIT_SESSION_MAX` with defaults 600 and 3000.

- [ ] **Step 1: Write failing workflow contract tests**

Parse `.github/workflows/ci.yml` and `docker-compose.yml` from Go. Assert that
RL-05 is invoked on pull requests, PF-09 invokes the non-skipping gate on a
schedule, PF-09 supplies both high-limit variables, and Compose maps those
variables to the Transmite flags while retaining defaults 600/3000.

- [ ] **Step 2: Run contract tests and verify RED**

Run `go test ./pkg/contracts -run TestCIGates -count=1`. Expected: failure because
the jobs and Compose overrides are absent.

- [ ] **Step 3: Add dedicated jobs and rate-limit configuration**

Give each job checkout, Go/protoc setup, full-stack startup, service wait,
protobuf generation, dependency download, its exact Make target, and
`if: always()` teardown. PF-09 sets limits above its generated ten-second load;
normal Compose startup uses 600/3000 defaults.

- [ ] **Step 4: Verify workflow contracts and test discovery**

Run the contract test, parse the workflow with a YAML parser, run `make -n
test-reliability` and `make -n test-perf-cache-gate`, and compile the reliability
and perf tagged packages. Expected: no skip-only target is used and all commands
exit zero.

- [ ] **Step 5: Commit**

Commit message: `ci: enforce cache reliability and performance gates`.

### Task 4: Final review and PR update

**Files:**
- Modify only files required by review findings.

- [ ] **Step 1: Run branch verification**

Run fresh protobuf generation, default Go tests, tagged vet/compile checks,
targeted race tests, Redis Cluster harnesses, workflow contract tests, YAML
parsing, and `git diff --check`.

- [ ] **Step 2: Review the complete branch diff**

Check requested-change compliance, concurrency correctness, Redis Cluster hash
slot safety, failure handling, and test claims. Fix every Critical or Important
finding and rerun its covering tests.

- [ ] **Step 3: Push and update GitHub threads**

Push the reviewed commits. Reply to each inline thread with the concrete fix and
verification, then resolve only threads whose requested behavior is fully met.
