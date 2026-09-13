# Technology Stack and Entry Points

Target version: `3.0-dev`
Status: Current
Verified: 2026-09-13

Use this reference for the `3.0-dev` architecture line, then verify task-sensitive details at the resolved commit.

## Stack

| Concern | Technology | Primary evidence |
|---|---|---|
| Production language | C++17 | Service `CMakeLists.txt` files |
| Build | CMake | `CMakeLists.txt`; `<service>/CMakeLists.txt` |
| External API | HTTP with Protobuf payloads | `gateway/source/gateway_server.h`; `proto/common/envelope.proto` |
| Internal RPC | brpc and Protobuf | `proto/*/*_service.proto`; service source |
| Long-lived client delivery | WebSocket | `push/source/push_server.h`; `push/source/connection.hpp` |
| Database | MySQL 8 with ODB | `odb/`; `common/dao/mysql*.hpp`; service CMake files |
| Cache and coordination | Redis 7 Cluster plus local L1 caches | `common/dao/data_redis.hpp`; `common/utils/local_cache.hpp` |
| Message broker | RabbitMQ through AMQP-CPP/libev | `common/mq/`; Transmite, Message, and Push source |
| Search | Elasticsearch 7 | `common/dao/data_es.hpp`; Message, Identity, Relationship source |
| Object storage | MinIO through the S3-compatible AWS C++ SDK | `common/infra/s3_client.hpp`; Media source; root `docker-compose.yml`; `conf/docker/media.json` |
| Service discovery and leases | etcd | `common/infra/etcd.hpp`; service entry files |
| Authentication | JWT HS256 with multi-key rotation support | `common/auth/`; `conf/auth.json` |
| Logging | spdlog JSON lines with propagated trace context | `common/infra/logger.hpp`; `common/log/`; `gateway/source/gateway_trace.hpp` |
| Integration and system tests | Go | `tests/go.mod`; `tests/bvt`; `tests/func`; `tests/perf` |
| Automation | GitHub Actions | `.github/workflows/ci.yml` |

## Build entry points

The CI Reliability job selects `tests/compose/reliability.yml` in addition to root Compose. That isolated test-only network has explicit IPAM so the Identity endpoint fault can request and restore IPv4 addresses on supported Docker Engine versions; it does not change the normal development network. See the testing framework for subnet overrides and preflight behavior.

`common/mq/channel.hpp` uses direct brpc initialization for numeric endpoints and `Init("http://<hostname>:<port>", "rr", options)` for DNS endpoints. Here `http://` selects the DNS naming service, not the wire protocol: internal RPC remains `baidu_std`. The pinned brpc implementation refreshes DNS every five seconds by default (`ns_access_interval`), including when the etcd registration string is unchanged. Connection timeout, RPC timeout and retry limits remain in `ServiceChannel`; literal IPv4/IPv6 endpoints keep the existing direct path. References: [brpc client naming services](https://brpc.apache.org/docs/client/basics/), [pinned periodic refresh implementation](https://github.com/apache/brpc/blob/041cec5fb84a5b4458bac6275ea7d34e048bc3f1/src/brpc/periodic_naming_service.cpp).

`Registry` in `common/infra/etcd.hpp` owns registration recovery. The pinned [etcd-cpp KeepAlive implementation](https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3/blob/ba6216385fc332b23d95683966824c2b86c2474e/src/KeepAlive.cpp) stops after a terminal error, while its [stream creation/cancellation](https://github.com/etcd-cpp-apiv3/etcd-cpp-apiv3/blob/ba6216385fc332b23d95683966824c2b86c2474e/src/v3/AsyncGRPC.cpp) includes unbounded completion-queue waits. Registry therefore uses only deadline-bound unary operations: every ten seconds it grants a fresh 30-second lease, publishes the remembered key/value under it, then revokes the previous lease. This trades up to three operations and one repeated PUT per successful cycle for explicit timeout and shutdown control; `ServiceChannel::append` deduplicates unchanged host values, retaining caller channels. Each operation has a two-second deadline, failed cycles retry after one second, and an unsuccessful candidate is revoked or expires naturally. `unregister()` is idempotent and permanently disables this Registry, joins recovery, and revokes the owned lease rather than deleting an unconditionally named key. Wire contracts, key paths, registration values and lease TTL are unchanged; lease IDs now rotate during normal operation, and older instances still lack recovery until upgraded. This does not claim complete service shutdown bounds for other unrelated worker/discovery threads or production-scale performance.

The root CMake project adds only the nine services. `common/test/` is retained as historical source and is not part of the maintained build or test graph; do not restore those C++ targets as behavioral coverage. The default Release build used by CI is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Inspect root `CMakeLists.txt`, the affected service's `CMakeLists.txt`, and its `Dockerfile` when changing dependencies, generated Protobuf inputs, linking, or runtime packaging.

BLD-01 in `tests/pkg/buildcontract` configures a real temporary Release tree and inspects CMake's file-API codemodel for exactly the nine service targets. Run `cd tests && CHATNOW_CMAKE_SOURCE="$(cd .. && pwd)" go test ./pkg/buildcontract -v -count=1` on a host with native dependencies. CI compiles this standard-library-only Go test and runs it in the pinned native builder, then builds the default target and validates all nine packaged service artifacts. The graph assertion alone does not prove compilation or service behavior.

## Configuration and runtime entry points

- Local service flags: `conf/local/*_server.conf`.
- Container service flags: `conf/docker/*_server.conf`.
- JWT keys and TTLs: Identity, Gateway, and Push resolve `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` at process startup. The value is the complete JSON document.
- Media S3 application credentials are resolved through the common secret resolver. `conf/media.json` is the local-process configuration; root Compose mounts `conf/docker/media.json` read-only. Container-internal S3 operations use `http://minio:9000`, while client-facing local presigned URLs use the published `http://127.0.0.1:19000` endpoint.
- Example Transmite flags: `conf/transmite_server.conf.example`.
- Service defaults and flag definitions: each `<service>/source/<service>_server.cc`.
- MySQL passwords for Conversation, Identity, Media, Message, and Relationship use service-specific direct-environment or `_FILE` inputs through `common/config/secret_resolver.hpp`.
- RabbitMQ passwords for Transmite, Message, and Push use the same resolver contract. Identity SMTP and Media S3 application credentials are also migrated.
- The `mq_host` flag is host-only for Transmite, Message, and Push. Their builders append the fixed AMQP port `5672`; configuration must not include a port.
- The resolver accepts exactly one allowlisted direct environment variable or `_FILE` locator, fails closed on missing/conflicting input, and validates secret-file type, owner, mode, size, and content. It reads once at startup; there is no hot reload.
- Bootstrap credentials in Compose remain deployment environment references rather than application resolver inputs. Redis has no configured password or ACL consumer.
- The canonical names, consumers, deployment rules, and limitations are maintained in `docs/operations/runtime-secrets.md`.
- Root `docker-compose.yml` is the integrated local application topology. It includes health-checked MySQL, six-node Redis Cluster, RabbitMQ, Elasticsearch, etcd, and MinIO plus one-shot `mysql-init`, `redis-cluster-init`, `rabbitmq-init`, and `minio-init` convergence services. Application dependencies use health or `service_completed_successfully` conditions instead of fixed startup delays.
- `mysql-init` mounts `sql/` read-only, applies ordered `V*.sql` files through a checksum ledger, rejects changes to an applied version, covers the 17 current ODB object tables, and converges five table-scoped MySQL application users. This is a forward-only repository bootstrap mechanism, not a general rollback engine.
- MinIO S3 and console ports are published on loopback `19000` and `19001`; Gateway HTTP remains `9000` and Push WebSocket remains `9001`. The supplemental `docker/docker-compose.yml` is not part of the root topology and must not be combined with it.
- `scripts/wait_for_services.sh` is the bounded semantic cross-stack readiness entry point. It checks Redis Cluster state and slots, the MySQL schema and users, RabbitMQ alarms, Elasticsearch health, MinIO health and buckets, eight exact etcd registrations, dependency-aware Gateway health, and Push listener reachability. The shared service `entrypoint.sh` performs bounded TCP prerequisite polling only.
- Readiness requires GNU `timeout` from coreutils: Docker Compose probes receive only the remaining deadline budget and a one-second kill grace period. The MinIO bucket probe passes bootstrap credentials on stdin inside a temporary container, not in process arguments.

The source topology and static contracts do not prove a successful clean-slate start. Root infrastructure state uses bind mounts under `middle/data`, so `docker compose down -v` does not remove it. Issue #88 integrates the runtime into CI using a fresh checkout per job and synthetic credentials; until fresh dynamic evidence exists, report full-stack cold start and Go runtime gates as unverified. The canonical operating contract is `docs/operations/compose-runtime.md`.

## Verification entry points

The current test framework is entirely Go. New or restored C++ test suites are prohibited.

| Layer | Location/tag | Runnable entry point |
|---|---|---|
| L0 Build | CI | CMake build, `cd tests && go vet ./...`, and `gofmt -l tests` from the repository root |
| L1 BVT | `tests/bvt`, `bvt` | `cd tests && make proto && make test-bvt` |
| L2 Functional | `tests/func`, `func` | `cd tests && make proto && make test-func` |
| L3 Scenario | `tests/func`, `func` | `cd tests && make proto && make test-scenario` |
| L4 Performance | `tests/perf`, `perf` | `cd tests && make proto && make test-perf` |
| Reliability | `tests/reliability`, `reliability` | `cd tests && make proto && make test-reliability` |

Reliability exercises Redis circuit recovery and Push unacked requeue, a scoped Identity endpoint move, Message stop/start, and Identity lease expiry through the controllers in `tests/pkg/chaos`. The lease controller suspends Identity until its exact registration disappears, then resumes the same process; it requires account RPC recovery without restarting Identity, Gateway or Transmite. The Make target runs the whole layer and does not consume `TEST_RUN`; use a direct tagged `go test ... -run` command when exact selection is required. No current controller covers RabbitMQ, MySQL, arbitrary services, or general network faults.

The CI definition has a dedicated `reliability` job that depends on `service-artifacts`, independently of BVT. The job uses a dedicated test stack with bounded RL-05 limits. Report the actual current-head runtime result; the existence of a job is not green runtime evidence. Shared clients, fixtures, polling, cleanup, and direct store verification live under `tests/pkg`.

The CI definition is `.github/workflows/ci.yml`; verify its commands against files present at the target commit before copying them into local instructions.
