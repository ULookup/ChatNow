# ChatNow Test Framework

Read this reference before selecting, implementing, running, or reporting a test. Reinspect `tests/Makefile`, `.github/workflows/ci.yml`, and the current `tests/` tree before relying on a path, target, or command; executable files are authoritative.

## Layers and executable surface

| Layer | Current location/tag | Purpose | Runnable repository command |
|---|---|---|---|
| L0 Build | `.github/workflows/ci.yml`; no test tag | C++ build, Go vet, Go formatting | Use the build workflow commands below. |
| L1 BVT | `tests/bvt`, `bvt` | Fast core-path smoke gate | `make -C tests test-bvt` |
| L2 Functional | `tests/func`, `func` | Service APIs, errors, boundaries | `make -C tests test-func` |
| L3 Scenario | `tests/func`, `func`; `TestScenario` filter | Cross-service workflows and store consistency | `make -C tests test-scenario` |
| L4 Performance | `tests/perf`, `perf` | Throughput and latency baselines | `make -C tests test-perf` |
| Reliability | Reserved `reliability` tag and distinct layer | Failure injection, recovery, durability, and convergence | No current directory or Make target. Inspect the executable surface; do not invent a command or claim a run. |

The current `tests/Makefile` also provides `make -C tests proto`, `make -C tests deps`, and `make -C tests clean`. Run `proto` only when generated Go protobuf is required; `clean` removes generated `tests/proto/chatnow` content.

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

CI orders `build` -> `bvt` -> `func`; a failed BVT prevents Functional and Scenario execution. Scheduled runs continue from `func` to `perf`. Preserve BVT short-circuit behavior when changing workflows or selecting local risk checks.

## Shared framework

- `tests/pkg/client`: configuration plus shared HTTP and WebSocket clients. Use `client.NewRequestID()` and `client.NewDeviceID()` for collision-resistant request, idempotency, device, and test-data suffixes.
- `tests/pkg/fixture`: reusable authenticated users, friendships, conversations, groups, messages, media, and WebSocket setup. Extend a fixture instead of copying setup.
- `tests/pkg/cleanup`: stack-readiness polling and suite cleanup. `tests/bvt/setup_test.go` and `tests/func/setup_test.go` call it from `TestMain`.
- `tests/pkg/verify`: direct MySQL, Elasticsearch, and MinIO checks. Use these when an API success alone cannot prove persistence, indexing, object state, idempotency, or cross-store convergence.

Use unique IDs for every request and collision-prone resource. Do not rely on a fixed username, message idempotency key, group name, device ID, file key, or search token shared across runs.

## Waiting and cleanup

Poll the externally observable condition with a bounded deadline and useful failure message. Suitable conditions include service reachability, a WebSocket event, a database row/state, an Elasticsearch hit, a MinIO object, or an API state transition. A polling interval is allowed; a fixed delay used as proof of readiness or convergence is not.

Assign exactly one owner for each created resource. Prefer suite-level cleanup through `tests/pkg/cleanup`; add `t.Cleanup` for per-test resources such as clients, sockets, temporary objects, or state that suite cleanup cannot safely own. Cleanup must run on assertion failure. CI owns `docker compose down -v` in its full-stack jobs.

## Change-to-layer matrix

| Change | Minimum RED layer | Risk escalation |
|---|---|---|
| Build/configuration shape with no runtime semantics | L0 | Use the sole behavior-neutral exemption only with proof. |
| Core happy path or stack boot contract | L1 BVT | Then L2 when service behavior or error handling also changes. |
| One service API, authorization rule, validation, boundary, or error path | L2 Functional | Add L3 when other services or stores participate. |
| Cross-service flow, MQ/WebSocket delivery, idempotency, ordering, or MySQL/Elasticsearch/MinIO consistency | L3 Scenario | Also run affected L2 and L1 gates. |
| Throughput, latency, allocation, or benchmark threshold | L4 Performance | Also run correctness layers for behavior used by the benchmark. |
| Failure injection, restart recovery, durability, or degraded convergence | Reliability | Treat as reserved until an executable surface exists; add the necessary architecture only when the scoped change authorizes it, and run lower correctness layers meanwhile. |

Choose the lowest layer that can fail for the required behavior, not the cheapest layer that happens to run. Run the target test first, its same-layer regressions second, and broader layers according to risk.

## Reporting unavailable dynamic tests

State `NOT RUN`, name the exact intended existing command, and explain the missing Linux/full-stack dependency. Report completed static checks separately. Do not write `passed`, `verified`, or equivalent for an unexecuted dynamic test.
