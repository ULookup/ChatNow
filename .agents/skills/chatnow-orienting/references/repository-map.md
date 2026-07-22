# Repository Map

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

## Ownership and first reads

| Path | Ownership | First-read files |
|---|---|---|
| `common/` | Shared auth, DAO, errors, infrastructure, MQ, logging, and utilities | `common/auth/auth_context.hpp`, `common/dao/data_redis.hpp`, `common/error/handle_rpc.hpp`, `common/infra/etcd.hpp`, `common/mq/channel.hpp` |
| `proto/` | External and internal Protobuf contracts | `proto/common/envelope.proto`, `proto/common/auth/metadata.proto`, affected `proto/*/*_service.proto` |
| `gateway/` | HTTP entry, JWT validation, discovery-based routing, response envelopes | `gateway/source/gateway_server.h`, `gateway/source/gateway_auth.hpp`, `gateway/source/gateway_server.cc`, `gateway/CMakeLists.txt` |
| `identity/` | Registration, login/logout, token issue/refresh, profile and lookup | `identity/source/identity_server.h`, `identity/source/identity_server.cc`, `proto/identity/identity_service.proto` |
| `relationship/` | Friend requests, relationships, blocking | `relationship/source/relationship_server.h`, `relationship/source/relationship_server.cc`, `proto/relationship/relationship_service.proto` |
| `conversation/` | Conversation lifecycle, membership, unread, visibility, pins, drafts | `conversation/source/conversation_server.h`, `conversation/source/conversation_server.cc`, `proto/conversation/conversation_service.proto` |
| `transmite/` | Message authorization, sequencing, idempotency, MQ publication | `transmite/source/transmite_server.h`, `transmite/source/transmite_server.cc`, `proto/transmite/transmite_service.proto` |
| `message/` | Message persistence, timelines, sync/history, read-ACK convergence, search indexing | `message/source/message_server.h`, `message/source/message_server.cc`, `proto/message/message_service.proto`, `proto/message/message_internal.proto` |
| `media/` | Presigned upload/download, multipart, dedup, quota, metadata, cleanup, speech | `media/source/media_server.h`, `media/source/upload_handler.hpp`, `media/source/download_handler.hpp`, `proto/media/media_service.proto` |
| `presence/` | Presence aggregation, subscriptions, and typing coordination | `presence/source/presence_server.h`, `presence/source/presence_server.cc`, `proto/presence/presence_service.proto` |
| `push/` | WebSocket connections, routes, cross-instance delivery, resend, client ACK ingestion | `push/source/push_server.h`, `push/source/connection.hpp`, `push/source/push_server.cc`, `proto/push/notify.proto` |
| `odb/` | ODB entity definitions and durable relational fields | Affected entity, especially `message.hxx`, `user_timeline.hxx`, `conversation_member.hxx`, and `media_*.hxx` |
| `conf/` | Non-secret local/container flags and JSON configuration; tracked files are not a runtime secret source | `conf/local/`, `conf/docker/`, `conf/auth.json`, `conf/media.json` |
| `sql/` | Versioned schema migrations | `sql/V4__media.sql` and any migration matching affected ODB entities |
| `docker/` | Separate MinIO topology and initialization; not wired into the root application network | `docker/docker-compose.yml`, `docker/minio-init/entrypoint.sh` |
| `docker-compose.yml` | Application stack declaration; Media object-storage wiring is incomplete | Root `docker-compose.yml`, then affected `Dockerfile` and `conf/docker` file |
| `scripts/` | Operational support and monitoring configuration | `scripts/install_aws_sdk_linux.sh`, `scripts/prometheus/redis_alerts.yml` |
| `tests/` | Pure-Go L1-L4 plus Redis-focused Reliability framework, clients, fixtures, cleanup, and store verification | `tests/Makefile`, `tests/config.yaml`, affected `tests/bvt`, `tests/func`, `tests/perf`, `tests/reliability`, `tests/pkg` |
| `docs/` | Secondary architecture/API context and canonical operations guidance | `docs/operations/runtime-secrets.md`, affected `docs/api/*.yaml`, then relevant architecture documents |

## Verified ports and infrastructure endpoints

Application ports come from `conf/local`, `conf/docker`, and root `docker-compose.yml`. MinIO ports come from the separate `docker/docker-compose.yml`; this is not an integrated container endpoint map.

| Owner | Local endpoint/port | Container endpoint/port | Evidence |
|---|---:|---:|---|
| Gateway HTTP | `127.0.0.1:9000` | `gateway_server:9000` | `gateway_server.conf` `http_listen_port` |
| Media brpc | `127.0.0.1:10002` | `media_server:10002` | `media_server.conf` |
| Identity brpc | `127.0.0.1:10003` | `identity_server:10003` | `identity_server.conf` |
| Transmite brpc | `127.0.0.1:10004` | `transmite_server:10004` | `transmite_server.conf` |
| Message brpc | `127.0.0.1:10005` | `message_server:10005` | `message_server.conf` |
| Relationship brpc | `127.0.0.1:10006` | `relationship_server:10006` | `relationship_server.conf` |
| Conversation brpc | `127.0.0.1:10007` | `conversation_server:10007` | `conversation_server.conf` |
| Push brpc | `127.0.0.1:10008` | `push_server:10008` | `push_server.conf` |
| Push WebSocket | `127.0.0.1:9001` | `push_server:9001` | `push_server.conf` `ws_port` |
| Presence brpc | `127.0.0.1:9050` | `presence_server:9050` | `presence_server.conf` |
| etcd | `127.0.0.1:2379` | `etcd:2379` | all service configs |
| MySQL | `127.0.0.1:3306` | `mysql:3306` | root Compose |
| Redis cluster | `127.0.0.1:6379`, `:6380`-`:6384` | `redis-node1:6379`, `redis-node2:6380` through `redis-node6:6384` | root Compose; service seed flags |
| RabbitMQ | `127.0.0.1:5672` | `rabbitmq:5672` | Transmite, Message, Push configs |
| Elasticsearch | HTTP `127.0.0.1:9200`; transport `:9300` | `elasticsearch:9200`; transport `:9300` | root Compose; service configs |
| MinIO S3 | Host `127.0.0.1:9000`, conflicting with Gateway | `minio:9000` only inside the separate MinIO Compose network | `conf/media.json`; supplemental Compose |
| MinIO console | Host `127.0.0.1:9001`, conflicting with Push WebSocket | `minio:9001` only inside the separate MinIO Compose network | supplemental Compose |

MySQL service configs set `mysql_port=0`, while root Compose exposes MySQL on `3306` and service entrypoints wait on `mysql:3306`; preserve that distinction when diagnosing driver defaults. Gateway's `websocket_listen_port=0` is not the client WebSocket endpoint; Push owns `ws_port=9001`.

Root Compose mounts `conf/media.json` into Media, but `s3.endpoint=http://127.0.0.1:9000` addresses the Media container itself. Root Compose has no MinIO service/dependency, while the supplemental MinIO Compose project has no declared shared external network with the root project. Do not present these declarations as a working integrated Media topology or recommend their current commands as a functional Media runtime. Any repair must explicitly reconcile the network, endpoint, dependency, and `9000`/`9001` host-port conflicts, then be verified from the affected containers.

## Runtime credential ownership

Current consumers at the verified commit are:

- Identity, Gateway, and Push resolve the complete JWT JSON document from `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` at startup. Identity signs and verifies tokens; Gateway and Push verify them.
- Conversation, Identity, Media, Message, and Relationship resolve service-specific MySQL password inputs through `common/config/secret_resolver.hpp`.
- Transmite, Message, and Push resolve service-specific RabbitMQ password inputs through the same resolver.
- Identity resolves its SMTP password; Media resolves separate S3 access-key and secret-key inputs. Non-secret S3 settings remain in `conf/media.json`.
- Root Compose requires MySQL, RabbitMQ, and supplemental MinIO bootstrap values through deployment environment references. These are separate from least-privileged application inputs. Redis has no configured password or ACL consumer.

Tracked runtime credential literals have been removed from the scoped source, configuration, Compose, and test-runtime surfaces. Do not reintroduce values in documentation, logs, test output, Issues, or PRs. Synthetic test-only credentials and API examples require narrow scanner exemptions rather than broad path allowlists.

The canonical current inventory and injection contract are in `docs/operations/runtime-secrets.md`. Reinspect the resolver and each consumer before extending the allowlist or claiming support for a credential not named there.

## State ownership

- MySQL/ODB: durable users, relationships, conversations/members, messages/timelines, ACK high-water marks, and media metadata/quota.
- Message is the durable source of truth for stored messages.
- Redis: JWT revocation/refresh state, sequences, idempotency, routes, presence, subscriptions, typing, unacked delivery, outboxes, and caches. Confirm the exact key in `common/dao/data_redis.hpp`.
- RabbitMQ: at-least-once message persistence, push, and indexing boundaries; publishers use confirms where implemented.
- Elasticsearch: derived search index, not the message source of truth.
- MinIO: media objects; MySQL holds file ownership/status/quota metadata.
- etcd: discovery, leases, worker allocation, and leader election.
