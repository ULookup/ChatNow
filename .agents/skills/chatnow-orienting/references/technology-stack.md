# Technology Stack and Entry Points

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
- JWT keys and TTLs: `conf/auth.json`.
- Media S3, buckets, presign, and MIME policy: `conf/media.json` plus Media flags.
- Example Transmite flags: `conf/transmite_server.conf.example`.
- Service defaults and flag definitions: each `<service>/source/<service>_server.cc`.
- Root `docker-compose.yml` is the full application/dependency topology used by CI. Start it with `docker compose up -d --build` when its required environment credentials are configured.
- `docker/docker-compose.yml` separately defines MinIO and its initialization sidecar. Run it with `docker compose -f docker/docker-compose.yml up -d minio minio-init`; do not infer the application stack from this supplemental file.

The local application ports and the MinIO host ports overlap: Gateway HTTP and MinIO S3 both use `9000`; Push WebSocket and the MinIO console both use `9001`. Choose a non-conflicting topology when running both.

## Verification entry points

The current test framework is entirely Go. New or restored C++ test suites are prohibited.

| Layer | Location/tag | Runnable entry point |
|---|---|---|
| L0 Build | CI | CMake build, `cd tests && go vet ./...`, and `gofmt -l tests` from the repository root |
| L1 BVT | `tests/bvt`, `bvt` | `cd tests && make proto && make test-bvt` |
| L2 Functional | `tests/func`, `func` | `cd tests && make proto && make test-func` |
| L3 Scenario | `tests/func`, `func` | `cd tests && make proto && make test-scenario` |
| L4 Performance | `tests/perf`, `perf` | `cd tests && make proto && make test-perf` |

Reliability is a distinct framework layer with the reserved `reliability` build tag. There is currently no repository path or Make target for it, so do not claim a runnable Reliability command. Shared clients, fixtures, polling, cleanup, and direct store verification live under `tests/pkg`.

The CI definition is `.github/workflows/ci.yml`; verify its commands against files present at the target commit before copying them into local instructions.
