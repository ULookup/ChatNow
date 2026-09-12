# ChatNow Test Framework

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

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

The Reliability target runs the complete tagged package and does not consume `TEST_RUN`. For an exact test, invoke the same tagged Go package with an anchored `-run` expression, then run `make -C tests test-reliability` for the layer regression. The current fault controller is Redis-only; there is no RabbitMQ, MySQL, arbitrary-service, or general-network controller.

The L0 commands currently encoded in `.github/workflows/ci.yml` are:

```bash
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
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

The dedicated Reliability job exists, but the inspected PR run was skipped after an upstream failure. Its existence is executable-surface evidence, not a successful runtime result.

## Full-stack readiness boundary

`scripts/wait_for_services.sh` is the bounded pre-suite gate for the root Compose runtime when a full-stack job or operator explicitly invokes it. It verifies Redis Cluster state and slots, all 17 ODB tables and five MySQL application users, RabbitMQ running/alarm state, Elasticsearch yellow-or-green health, MinIO readiness and both media buckets, eight exact etcd service registrations, dependency-aware Gateway `GET /health`, and Push listener reachability. The Push check is TCP reachability, not WebSocket delivery evidence.

Container health, one-shot initializer completion, and the shared `entrypoint.sh` bounded TCP polling are startup prerequisites; none replaces `scripts/wait_for_services.sh`. Conversely, a passing static contract for the helper or Compose shape does not prove that any container started or that a runtime gate passed.

Issue #88 integrates the Issue #78 runtime into CI. Each job owns a fresh checkout and its middle/data directory. scripts/create_test_env.py generates synthetic credentials and refuses existing state. Until a fresh run exists for the exact commit, report cold start, BVT, Functional, Reliability, and Performance as `not run` or `blocked`, not passed.

## Shared framework

- `tests/pkg/client`: configuration plus shared HTTP and WebSocket clients. Use `client.NewRequestID()` and `client.NewDeviceID()` for collision-resistant request, idempotency, device, and test-data suffixes.
- `tests/pkg/fixture`: reusable authenticated users, friendships, conversations, groups, messages, media, and WebSocket setup. Extend a fixture instead of copying setup.
- `tests/pkg/cleanup`: stack-readiness polling and suite cleanup. `tests/bvt/setup_test.go` and `tests/func/setup_test.go` call it from `TestMain`.
- `tests/pkg/verify`: direct MySQL, Elasticsearch, and MinIO checks. Use these when an API success alone cannot prove persistence, indexing, object state, idempotency, or cross-store convergence.

Use unique IDs for every request and collision-prone resource. Do not rely on a fixed username, message idempotency key, group name, device ID, file key, or search token shared across runs.

## Waiting and cleanup

Poll the externally observable condition with a bounded deadline and useful failure message. Suitable conditions include service reachability, a WebSocket event, a database row/state, an Elasticsearch hit, a MinIO object, or an API state transition. A polling interval is allowed; a fixed delay used as proof of readiness or convergence is not.

Assign exactly one owner for each created resource. Prefer suite-level cleanup through `tests/pkg/cleanup`; add `t.Cleanup` for per-test resources such as clients, sockets, temporary objects, or state that suite cleanup cannot safely own. Cleanup must run on assertion failure. Root Compose persists infrastructure through bind mounts under `middle/data`; `docker compose down -v` does not remove that state. A clean-slate test must use an explicitly disposable storage path or the separate CI override and must never delete a shared tree.

## Change-to-layer matrix

| Change | Minimum RED layer | Risk escalation |
|---|---|---|
| Build/configuration shape with no runtime semantics | L0 | Use the sole behavior-neutral exemption only with proof. |
| Core happy path or stack boot contract | L1 BVT | Then L2 when service behavior or error handling also changes. |
| One service API, authorization rule, validation, boundary, or error path | L2 Functional | Add L3 when other services or stores participate. |
| Cross-service flow, MQ/WebSocket delivery, idempotency, ordering, or MySQL/Elasticsearch/MinIO consistency | L3 Scenario | Also run affected L2 and L1 gates. |
| Throughput, latency, allocation, or benchmark threshold | L4 Performance | Also run correctness layers for behavior used by the benchmark. |
| Redis failure injection, restart recovery, or Push unacked convergence | Reliability | Run the exact tagged test, then `make -C tests test-reliability`; also run lower correctness layers selected by the affected behavior. |
| RabbitMQ/MySQL/service/network fault behavior | Reliability | No current controller exists for these faults. Do not expand the framework unless the scoped Issue authorizes it; use the nearest executable correctness layer and report the gap. |

Choose the lowest layer that can fail for the required behavior, not the cheapest layer that happens to run. Run the target test first, its same-layer regressions second, and broader layers according to risk.

## Reporting unavailable dynamic tests

State `NOT RUN`, name the exact intended existing command, and explain the missing Linux/full-stack dependency. Report completed static checks separately. Do not write `passed`, `verified`, or equivalent for an unexecuted dynamic test.
