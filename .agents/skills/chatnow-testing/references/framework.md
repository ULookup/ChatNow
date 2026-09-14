# ChatNow Test Framework

Target version: `3.0-dev`
Status: Current
Verified: 2026-09-13

Read this reference before selecting, implementing, running, or reporting a test. Reinspect `tests/Makefile`, `.github/workflows/ci.yml`, and the current `tests/` tree before relying on a path, target, or command; executable files are authoritative.

## Layers and executable surface

| Layer | Current location/tag | Purpose | Runnable repository command |
|---|---|---|---|
| L0 Build | `.github/workflows/ci.yml`; no test tag | C++ build, Go vet, Go formatting | Use the build workflow commands below. |
| L1 BVT | `tests/bvt`, `bvt` | Fast core-path smoke gate | `make -C tests test-bvt` |
| L2 Functional | `tests/func`, `func` | Service APIs, errors, boundaries | `make -C tests test-func` |
| L3 Scenario | `tests/func`, `func`; `TestScenario` filter | Cross-service workflows and store consistency | `make -C tests test-scenario` |
| L4 Performance | `tests/perf`, `perf` | Throughput and latency baselines | `make -C tests test-perf` |
| Reliability | `tests/reliability`, `reliability` | Redis failure injection, recovery, and Push unacked convergence | `make -C tests test-reliability` |

The current `tests/Makefile` also provides `make -C tests proto`, `make -C tests deps`, `make -C tests test-agent-policy`, and `make -C tests clean`. Run `proto` only when generated Go protobuf is required; `clean` removes generated `tests/proto/chatnow` content. Repository policy checks and `tests/pkg/contracts` Compose/runtime contracts are static evidence and never substitute for a behavior RED or runtime gate. Repository contracts do not consume a BVT/Functional case ID namespace.

Reliability requires a dedicated disposable stack. From the repository root, export `COMPOSE_FILE=docker-compose.yml:tests/compose/reliability.yml`, then start it with `TRANSMITE_RATE_LIMIT_USER_MAX=8 TRANSMITE_RATE_LIMIT_SESSION_MAX=40 docker compose up -d --build`. Keep the same Compose files selected for tests and teardown. The override gives the test network an explicit IPv4 subnet (default `172.30.97.0/24`, overridable with `CHATNOW_RELIABILITY_SUBNET` before creating the stack); older Docker engines require this for endpoint move/restore operations. Do not retrofit an existing shared network. The limits make the RL-05 fallback burst deterministic; they are not production recommendations. Run Functional/BVT on their separate default-limit stacks. The Push test uses its issued device ID and restores Redis/paused containers on failure.

The executable RL-05 cases use Redis pause/unpause to retain DNS while Redis stops responding. They prove process-stall circuit/recovery behavior and Unacked ordering, not container stop/recreate, DNS withdrawal, cluster topology changes or rolling recovery. Compose fault commands have a 20-second execution limit.

RL-05 preserves the bounded Gateway outage burst but measures fast rejection directly at one isolated Transmite endpoint. Gateway has its own Redis circuit, so its full HTTP duration is not a Transmite Open-state measurement. The pure-Go `DoInternalRPC` helper implements bounded, uncompressed `baidu_std` frames and forwards synthetic fixture `RpcMetadata`; `make proto` includes `common/auth/metadata.proto`. This uses the existing trusted test-network boundary and does not establish internal authentication or a public client API.

The test deliberately waits past the one-second Open interval to exercise a recovery probe; the delay is fault scheduling, not proof of readiness. For each direct request it records elapsed time separately from bvar reads. Increased connection-failure and open counters identify an admitted failed recovery attempt. Zero connection failures, unchanged open generation, and increased rejections identify an Open rejection; every such sample must take less than 50 ms. A bounded eight-request set must include a recovery probe and three Open rejection samples. Slow qualifying samples fail immediately and cannot be discarded by retrying. Run these tests without concurrent traffic on the same stack.

RL-05 cleanup unpauses Redis, waits for cluster health, then polls actual account and message RPCs with a 30-second convergence deadline. One recovery idempotency key bounds writes across retries. RL-REDIS-01 injects an assertion failure in a bounded child process, then requires its prepared parent's send to succeed immediately after cleanup. The subsequent Push test remains independent of residual Open circuits.

The Reliability target runs the complete tagged package and does not consume `TEST_RUN`. For an exact test, invoke the same tagged Go package with an anchored `-run` expression, then run `make -C tests test-reliability` for the layer regression. Current controllers cover Redis, the scoped Identity endpoint move and lease expiry, and Message stop/start; there is no RabbitMQ, MySQL, arbitrary-service, or general-network controller.

RL-MESSAGE-01 requires exclusive access to the disposable synthetic stack. `tests/pkg/chaos/message.go` stops only the Compose `message_server` with a five-second shutdown grace and registers idempotent restore before fault injection. The test creates one broker-confirmed message while Message is stopped, or stops after persistence, and exercises accepted/persisted guard formats using its unique synthetic sender and client ID. Direct Transmite RPC avoids masking its response behind Gateway's own timeout. Every success must retain the original nonzero identity and sequence; an unavailable result must have no partial message. After restore, it waits for the original durable row and requires successful retries, exactly one message, and one timeline row per member. Cleanup restores Message and waits for the queued write before the existing disposable-stack cleanup removes synthetic state, including on assertion failure. Do not run this test against shared data or concurrently with another suite. Run `go test -tags=reliability ./reliability/... -run '^TestRL_IdempotentResponseDuringMessageOutage$' -v -count=1 -timeout=120s` from `tests`, followed by the complete Reliability gate. FN-TM-02 covers a healthy lookup with no readable original and deletes only its synthetic guard. SC-06 explicitly requires recovery success after persistence instead of accepting a failed duplicate as sufficient evidence.

The L0 commands currently encoded in `.github/workflows/ci.yml` are:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace chatnow-ci-builder:ci bash -lc '
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)" --target conversation_server gateway_server identity_server media_server message_server presence_server push_server relationship_server transmite_server
'
cd tests && go vet ./...
cd tests
unformatted=$(gofmt -l .)
if [ -n "$unformatted" ]; then
  echo "$unformatted" >&2
  exit 1
fi
```

These are Linux workflow commands. On another platform, report the workflow as not run if its tools or full stack are unavailable. Never turn an adapted static check into a runtime claim.

## Gate order

CI runs `build` independently and builds reusable service artifacts. BVT needs `service-artifacts`; Functional needs both `service-artifacts` and BVT; Reliability needs `service-artifacts` but does not wait for BVT. Scheduled Performance runs after Functional, while the cache-performance job depends directly on `service-artifacts`. Preserve each gate's actual dependency when changing workflows or selecting local risk checks.

The dedicated Reliability job is executable. Its existence is not a successful runtime result; record the actual current-head result separately from BVT and Functional.

## Full-stack readiness boundary

`scripts/wait_for_services.sh` is the bounded pre-suite gate for the root Compose runtime when a full-stack job or operator explicitly invokes it. It verifies Redis Cluster state and slots, all 17 ODB tables and five MySQL application users, RabbitMQ running/alarm state, Elasticsearch yellow-or-green health, MinIO readiness and both media buckets, eight exact etcd service registrations, dependency-aware Gateway `GET /health`, and Push listener reachability. The Push check is TCP reachability, not WebSocket delivery evidence.

Container health, one-shot initializer completion, and the shared `entrypoint.sh` bounded TCP polling are startup prerequisites; none replaces `scripts/wait_for_services.sh`. Conversely, a passing static contract for the helper or Compose shape does not prove that any container started or that a runtime gate passed.

The readiness shell contracts in `tests/pkg/contracts/readiness_execution_test.go` execute the real helper with controlled external command boundaries. They verify stdin-only MinIO credentials and interruption of a stalled Docker probe within the deadline, without accessing real credentials or services. GNU `timeout` limits each Compose invocation to the remaining budget, followed by a one-second kill grace period. These process-boundary tests do not replace full-stack readiness or BVT.

Issue #88 integrates the Issue #78 runtime into CI. Each job owns a fresh checkout and its middle/data directory. scripts/create_test_env.py generates synthetic credentials and refuses existing state. Until a fresh run exists for the exact commit, report cold start, BVT, Functional, Reliability, and Performance as `not run` or `blocked`, not passed.

## Shared framework

Issue #58 extends the existing WebSocket client with bounded `SendNotify` writes and `fixture.WaitDeliveryACK`, which builds acknowledgements from real delivered frames. FN-WS-10/11/12 cover forged fields, optional watermarks, duplicates, out-of-order ACKs and Message business failures. FN-MS-02 uses `DBVerifier.HoldMemberExit` to stage a synthetic membership exit, observes a real MySQL lock waiter, then commits the exit before the ACK proceeds; it does not use a sleep to establish the race. The verifier registers rollback before the fault and uses bound SQL values.

RL-ACK-01 stops Message after a real delivery, submits an ACK, and verifies retained Redis state with no durable cursor advance during the established outage. It then restarts Push, reconnects the same authenticated device, requests heartbeat replay, and re-ACKs the original item after Message recovery. Every stop registers an idempotent restore before mutation; stack readiness precedes data cleanup. Run `go test -tags=reliability ./reliability/... -run '^TestRL_DeliveryAckRetainedDuringMessageOutage$' -v -count=1 -timeout=120s`, then the entire Reliability layer. Never overlap these faults with another suite.

PF-10 is an opt-in, sequential durable-ACK comparison: `CHATNOW_ACK_BENCH=1 go test -tags=perf ./perf/... -run '^$' -bench '^BenchmarkPF10_DeliveryACK$' -benchtime=100x -count=1 -timeout=180s`. It prepares real deliveries before timing, polls MySQL for durable convergence, and verifies final Redis removal outside the timed region. It reports observed ACK/s, P95/P99, and server-wide MySQL statements after subtracting known verifier reads. Polling resolution and background SQL affect these measurements; this is not a saturation, multi-instance capacity, or production SLO result. Use identical stack configuration for before/after samples. Counts above 200 are rejected; cleanup owns all synthetic state.

Issue #104 distinguishes message acceptance from fixture readiness. `fixture.SendTextMessage` submits exactly once, then uses `DBVerifier.WaitMessageExists` with a ten-second convergence budget before returning. The Message consumer commits the row, member timelines, and conversation watermark in one transaction. Message API tests delegate to this shared fixture; explicit client-ID and outage tests retain `SendTextMessageWithClientMsgId` or direct RPC when broker acceptance is the behavior under test. A failed send is never retried by the fixture. `TestSearchMessages_Success` waits for the exact synthetic message's Elasticsearch index entry and then requires a successful search containing that message; receiving a failure envelope is not a search success.

RL-MESSAGE-02 (`TestRL_TextFixtureWaitsForPersistence`) stops the disposable stack's Message consumer, starts one text fixture, observes that sender's `accepted` Redis guard, and checks that the fixture cannot return with zero durable rows. A 250 ms negative observation window applies only while the fault is established; recovery waits for actual MySQL state. Cleanup restores Message, joins the fixture goroutine, waits for the accepted row and service readiness, then removes synthetic data. Run `go test -tags=reliability ./reliability/... -run '^TestRL_TextFixtureWaitsForPersistence$' -v -count=1 -timeout=120s`, followed by the complete Reliability gate. This case proves fixture persistence timing, not the cause of historical RPC 9002 errors.

With `CHATNOW_TEST_DIAGNOSTICS=1`, the shared HTTP client emits one sanitized JSON event for each failed Protobuf response, preserves the original response, and performs no retry. It records success, business error code, header/request-ID presence, classified error text, numeric brpc codes, and a locally generated trace ID where the caller supplied none. Raw error text, response request IDs, authorization headers, and message bodies are omitted. `TestHTTPFailureDiagnostics` exercises this client boundary, including secret exclusion and exactly one request.

BVT, Functional and Reliability CI jobs capture initial process/registration state before the suite and collect failure state before teardown. `scripts/collect_test_diagnostics.py` reads only selected Docker process fields, exact etcd registration presence and filtered Gateway/Message/Transmite/Identity error events. Each subprocess has a two-second limit within a shared twenty-second budget. Unavailable evidence is explicitly marked, never inferred as a healthy service. The collector retains at most 500 events from bounded log tails; it never copies raw logs, container configuration, environment, registration values, user/device IDs or command stderr. Failed jobs upload `test-diagnostics` for seven days. Compare initial and failure timestamps/PIDs/restart counts; Reliability deliberately changes process state and must be interpreted with its fault schedule. This is diagnostic coverage, not a claim that an unreproduced historical crash is fixed.

RL-MESSAGE-01 intentionally starts Message again during recovery; it does not claim recovery of an expired registry lease in a surviving process (Issue #99). The recovery requests poll the actual duplicate response within ten seconds and preserve its original client ID. Initial and final stack-readiness checks reject a missing registration; a database row alone is not RPC-readiness evidence.

- `tests/pkg/client`: configuration plus shared HTTP and WebSocket clients. Use `client.NewRequestID()` and `client.NewDeviceID()` for collision-resistant request, idempotency, device, and test-data suffixes.
- `tests/pkg/fixture`: reusable authenticated users, friendships, conversations, groups, messages, media, and WebSocket setup. Extend a fixture instead of copying setup.
- `tests/pkg/cleanup`: stack-readiness polling and suite cleanup. `tests/bvt/setup_test.go` and `tests/func/setup_test.go` call it from `TestMain`.
- `tests/pkg/verify`: direct MySQL, Elasticsearch, and MinIO checks. Use these when an API success alone cannot prove persistence, indexing, object state, idempotency, or cross-store convergence.

Device fixtures must use the device ID issued by Identity. A second WebSocket payload using the same JWT does not create a second authenticated device; log in separately for each device. BVar reads request the brpc console representation and parse the named integer response. MinIO verification needs `MINIO_ENDPOINT` as well as the synthetic access credentials. Rate-limit correctness can initialize a test-owned exhausted bucket, while throughput belongs in Performance. Message persistence and search assertions wait for their bounded observable state rather than assuming the send response or index write implies immediate visibility.

Use unique IDs for every request and collision-prone resource. Do not rely on a fixed username, message idempotency key, group name, device ID, file key, or search token shared across runs.

`tests/pkg/fixture/jwt.go` signs temporal and invalid-signature variants of fixture-account JWTs for FN-AM-07. It requires `CHATNOW_JWT_CONFIG` from the isolated synthetic stack (CI exports the configuration generated by `scripts/create_test_env.py`). Supply the same synthetic environment to local Go tests; never use production keys. The fixture keeps keys and token contents out of diagnostics. The test checks Identity error codes and Gateway rejection, including 30-second, 120-second and one-day expiration, bad signatures, unknown keys, future `iat`/`nbf`, and valid-token controls. The older fixed invalid-token rejection test is named `TestJWTRequired_InvalidToken`; it is not expiration evidence.

## Waiting and cleanup

RL-WORKER-01 (`TestRL_WorkerLeaseLossFencesProcess`) uses `tests/pkg/chaos/worker.go` only on a dedicated synthetic Compose stack. It requires a single running Transmite and etcd in the same project and exactly one leased worker slot, records the original process and restart policy, registers cleanup before mutation, disables automatic restart, and revokes that sole lease. It observes the original process for up to 20 seconds and requires exit code 1, no OOM, and the fixed worker-loss diagnostic. Raw container logs remain in memory; test output contains only process/lease metadata and the diagnostic's presence. Cleanup restores the exact restart policy and starts the service on both RED and GREEN, followed by full-stack readiness. GREEN additionally observes the recovered process for seven seconds, covering five TTL-check intervals and a watchdog interval. Run `go test -tags=reliability ./reliability/... -run '^TestRL_WorkerLeaseLossFencesProcess$' -v -count=1 -timeout=120s`, then the complete Reliability layer and BVT/Functional/Scenario gates. Never overlap faults or suites on the same stack. This case does not prove arbitrary network-partition fencing, change the existing lease-loss detector, or cover allocator exhaustion.

Before moving Identity, the address controller creates an inert container from the already available Identity image, overrides its entrypoint with `/bin/true`, and checks an explicit address on the selected test network. It prefers high host addresses and allows at most 16 probes when Docker reports an address reservation absent from network inspection. It removes each probe before retrying or performing any live endpoint mutation. Unsupported networks therefore fail without disconnecting Identity. `TestAddressFaultRejectsUnsupportedNetworkBeforeDisconnect` exercises create/start rejection and an occupied-candidate retry across the Docker command boundary; the real Reliability case remains the runtime acceptance test.

RL-DISCOVERY-01 (`TestRL_DiscoveryRecoversAfterIdentityAddressChange`) uses `tests/pkg/chaos/address.go` to move only Identity's IPv4 endpoint in the selected isolated Compose project. It requires one Identity network owned by that project, preserves aliases, selects an unused address, bounds Docker commands, and restores the original address and aliases with `t.Cleanup`, including on RED. Identity is restarted at both the changed and restored addresses to reconnect its outgoing datastore clients and registry lease; Gateway and Transmite process identities must remain unchanged. Account/profile RPC and an uncached sender's message must recover within a shared 45-second budget. Run it with `go test -tags=reliability ./reliability/... -run '^TestRL_DiscoveryRecoversAfterIdentityAddressChange$' -v -count=1 -timeout=180s`, then the complete Reliability gate. Never run network fault tests concurrently with other suites on the same stack. This controller does not cover shared/external networks, IPv6 fault injection, registry lease recovery without a service restart, full container recreation or Redis address changes.

RL-DISCOVERY-02 (`TestRL_RegistryRecoversAfterLeaseExpiry`) uses `tests/pkg/chaos/lease.go` on a dedicated disposable stack. It records Identity, Gateway and Transmite process identities in one Compose project, pauses only Identity, and polls the exact `/service/identity_service/instance` key for up to 45 seconds until the real lease has expired. It then unpauses the same process and requires both registration and authenticated profile RPC within 60 seconds. Process preservation is checked before cleanup can repair a failed baseline by restarting Identity. Cleanup is registered before pause, restores the service and checks RPC convergence before deleting synthetic data. Run `go test -tags=reliability ./reliability/... -run '^TestRL_RegistryRecoversAfterLeaseExpiry$' -v -count=1 -timeout=150s` from `tests`, then the complete Reliability gate. This test proves lease expiry recovery, not a general network partition or all concurrent unregister schedules.

Poll the externally observable condition with a bounded deadline and useful failure message. Suitable conditions include service reachability, a WebSocket event, a database row/state, an Elasticsearch hit, a MinIO object, or an API state transition. A polling interval is allowed; a fixed delay used as proof of readiness or convergence is not.

Assign exactly one owner for each created resource. Prefer suite-level cleanup through `tests/pkg/cleanup`; add `t.Cleanup` for per-test resources such as clients, sockets, temporary objects, or state that suite cleanup cannot safely own. Cleanup must run on assertion failure. Root Compose persists infrastructure through bind mounts under `middle/data`; `docker compose down -v` does not remove that state. A clean-slate test must use a fresh CI checkout or another explicitly disposable storage path and must never delete a shared tree.

## Change-to-layer matrix

| Change | Minimum RED layer | Risk escalation |
|---|---|---|
| Build/configuration shape with no runtime semantics | L0 | Use the sole behavior-neutral exemption only with proof. |
| Core happy path or stack boot contract | L1 BVT | Then L2 when service behavior or error handling also changes. |
| One service API, authorization rule, validation, boundary, or error path | L2 Functional | Add L3 when other services or stores participate. |
| Cross-service flow, MQ/WebSocket delivery, idempotency, ordering, or MySQL/Elasticsearch/MinIO consistency | L3 Scenario | Also run affected L2 and L1 gates. |
| Throughput, latency, allocation, or benchmark threshold | L4 Performance | Also run correctness layers for behavior used by the benchmark. |
| Redis failure injection, restart recovery, or Push unacked convergence | Reliability | Run the exact tagged test, then `make -C tests test-reliability`; also run lower correctness layers selected by the affected behavior. |
| Identity hostname address recovery | Reliability | Use RL-DISCOVERY-01 and its scoped Compose endpoint controller, then lower correctness gates. |
| Expired service registration in a surviving process | Reliability | Use RL-DISCOVERY-02 and its scoped Identity lease controller, then the complete layer and lower correctness gates. |
| Other RabbitMQ/MySQL/service/network fault behavior | Reliability | No general controller exists for these faults. Do not expand the framework unless the scoped Issue authorizes it; use the nearest executable correctness layer and report the gap. |

Choose the lowest layer that can fail for the required behavior, not the cheapest layer that happens to run. Run the target test first, its same-layer regressions second, and broader layers according to risk.

## Reporting unavailable dynamic tests

State `NOT RUN`, name the exact intended existing command, and explain the missing Linux/full-stack dependency. Report completed static checks separately. Do not write `passed`, `verified`, or equivalent for an unexecuted dynamic test.
