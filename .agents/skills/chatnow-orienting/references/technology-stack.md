# Technology Stack and Entry Points

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

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
| Object storage | MinIO through the S3-compatible AWS C++ SDK | `common/infra/s3_client.hpp`; Media source; `docker/docker-compose.yml` |
| Service discovery and leases | etcd | `common/infra/etcd.hpp`; service entry files |
| Authentication | JWT HS256 with multi-key rotation support | `common/auth/`; `conf/auth.json` |
| Logging | spdlog JSON lines with propagated trace context | `common/infra/logger.hpp`; `common/log/`; `gateway/source/gateway_trace.hpp` |
| Integration and system tests | Go | `tests/go.mod`; `tests/bvt`; `tests/func`; `tests/perf` |
| Automation | GitHub Actions | `.github/workflows/ci.yml` |

## Build entry points

The root CMake project adds all nine services. The CI-equivalent build is:

```bash
cmake -S . -B build
cmake --build build -j
```

Inspect root `CMakeLists.txt`, the affected service's `CMakeLists.txt`, and its `Dockerfile` when changing dependencies, generated Protobuf inputs, linking, or runtime packaging.

## Configuration and runtime entry points

- Local service flags: `conf/local/*_server.conf`.
- Container service flags: `conf/docker/*_server.conf`.
- JWT keys and TTLs: Identity, Gateway, and Push resolve `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` at process startup. The value is the complete JSON document.
- Media S3 application credentials are resolved through the common secret resolver. Buckets, endpoint, presign, and MIME policy remain in `conf/media.json` plus Media flags.
- Example Transmite flags: `conf/transmite_server.conf.example`.
- Service defaults and flag definitions: each `<service>/source/<service>_server.cc`.
- MySQL passwords for Conversation, Identity, Media, Message, and Relationship use service-specific direct-environment or `_FILE` inputs through `common/config/secret_resolver.hpp`.
- RabbitMQ passwords for Transmite, Message, and Push use the same resolver contract. Identity SMTP and Media S3 application credentials are also migrated.
- The resolver accepts exactly one allowlisted direct environment variable or `_FILE` locator, fails closed on missing/conflicting input, and validates secret-file type, owner, mode, size, and content. It reads once at startup; there is no hot reload.
- Bootstrap credentials in Compose remain deployment environment references rather than application resolver inputs. Redis has no configured password or ACL consumer.
- The canonical names, consumers, deployment rules, and limitations are maintained in `docs/operations/runtime-secrets.md`.
- Root `docker-compose.yml` declares the application stack used by CI, but it is not a complete integrated Media/MinIO topology: it starts Media without a MinIO service or dependency.
- `docker/docker-compose.yml` separately declares MinIO and its initialization sidecar on a different default Compose network. Media mounts `conf/media.json`, whose `http://127.0.0.1:9000` endpoint resolves to the Media container itself, not to that separate MinIO container.

The two Compose declarations also conflict on host ports: Gateway HTTP and MinIO S3 both publish `9000`; Push WebSocket and the MinIO console both publish `9001`. Therefore, neither `docker compose up -d --build` nor running both Compose files as written proves a functional containerized Media flow. Treat the network, Media S3 endpoint, service dependency, and host-port mapping as unresolved executable contradictions that must be fixed and verified before documenting a working container runtime command.

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

Reliability is an executable, Redis-focused layer. Its current tests exercise Redis circuit recovery and Push unacked requeue behavior through `tests/pkg/chaos/redis.go`. The Make target runs the whole layer and does not consume `TEST_RUN`; use a direct tagged `go test ... -run` command when exact selection is required. No current controller covers RabbitMQ, MySQL, arbitrary services, or general network faults, so do not describe this as a broad chaos platform.

The CI definition has a dedicated `reliability` job that depends on `service-artifacts`, independently of BVT. At this verification date the job exists, but the inspected PR run was skipped after an upstream failure; that is not green runtime evidence. Shared clients, fixtures, polling, cleanup, and direct store verification live under `tests/pkg`.

The CI definition is `.github/workflows/ci.yml`; verify its commands against files present at the target commit before copying them into local instructions.
