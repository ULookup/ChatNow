# Core Flows

Target version: `3.0-dev`
Status: Current
Verified: 2026-09-13

These are current-state flows for the `3.0-dev` line. Re-verify affected symbols at the target commit and keep proposals in a separate section.

The release contract confirmed on 2026-09-13 preserves concurrent authenticated devices for one account. Logging in on another device must not invalidate the earlier device merely because it is newer. `DeleteMessages` removes only the authenticated caller's timeline references; it does not recall the shared message or remove another user's history. SC-07 and FN-MS-10 verify these boundaries with real sessions and persisted timelines.

## HTTP request flow

**Flow:** Client -> Gateway HTTP -> discovered brpc service -> Protobuf response envelope.

- Entry/contracts: `gateway/source/gateway_server.h`; affected `proto/*/*_service.proto`; `proto/common/envelope.proto`.
- Boundary: Gateway parses Protobuf, authenticates JWT except whitelisted Identity routes, derives `user_id`/`device_id`/JTI, adds trace context, and serializes `RpcMetadata` into the brpc attachment.
- Discovery: `ServiceManager` resolves service instances through etcd; calls are synchronous with route-specific timeouts.
- Failure: malformed input, unavailable backends, and RPC timeouts are translated into a Protobuf `ResponseHeader`; retries remain a client decision unless a flow states otherwise.
- Tests: `tests/bvt`, affected `tests/func`, `tests/func/auth_middleware_test.go`, `tests/func/security_test.go`.
- Invariants: client identity fields never override server-derived auth context; services validate required metadata; trace context propagates where supported.

## Message send and delivery

**Flow:** Client -> Gateway -> Transmite -> RabbitMQ message exchange -> Message/MySQL -> RabbitMQ push queue -> Push -> WebSocket.

- Entry/contracts: Gateway `/service/transmite/send`; `proto/transmite/transmite_service.proto`; `proto/message/message_internal.proto`; `proto/push/notify.proto`.
- Transmite: validates auth/membership and content, allocates conversation and per-user sequences in Redis, creates a Snowflake message ID, protects `client_msg_id` with a Redis idempotency key, and completes the brpc request after publisher confirm.
- Message: consumes `InternalMessage`, persists the message then per-user timelines in MySQL, treats duplicate message inserts idempotently, publishes push only after persistence, and asynchronously indexes text in Elasticsearch.
- Push: consumes the push event, resolves Redis/local routes, records per-device unacked payloads, sends locally or uses asynchronous cross-instance `PushBatch`, then WebSocket delivery reaches the client.
- Async/recovery: RabbitMQ and WebSocket delivery are at-least-once. Message uses Redis Push/ES outboxes and reapers for publish failures; Push uses a cross-instance outbox. Consumer redelivery and duplicate delivery require idempotent effects.
- Tests: `tests/bvt/message_test.go`, `tests/func/transmite_test.go`, `tests/func/message_test.go`, `tests/func/ws_notify_test.go`, `tests/func/scenarios_test.go`, `tests/perf/send_msg_test.go`, `tests/perf/sync_test.go`.
- Invariants: Message/MySQL is the stored-message source of truth; persistence precedes normal push publication; `request_id`, `client_msg_id`, message ID, conversation sequence, and user sequence have distinct roles; do not claim exactly-once delivery.

Message commits the conversation `max_seq` watermark in the same MySQL transaction as the message and timeline inserts. The watermark never decreases when deliveries arrive out of order; Conversation metadata writers preserve the latest committed value under a row lock. The Message database principal therefore needs `SELECT, UPDATE` on `conversation`.

Log-context storage must initialize in Release builds as well as Debug; initialization cannot depend on assertions. Transmite copies the authenticated RPC trace into MQ headers at publish time. Push invalidates its local route cache while binding a newly authenticated device under the same per-user lock used by route fills; it does not cache empty routes. Dismissed conversations are rejected by `GetMemberIds` before consulting cached membership.

Redis availability classification includes the pinned client's exhausted shard-refresh wrapper, while Redis command errors remain separate. An unavailable member snapshot has an unknown version: Transmite queries Conversation for that request and does not publish the result into L1/L2 without a version fence. Known-version races retain retry behavior. Sequence allocation and Unacked persistence remain Redis truth-source operations and fail unavailable during an outage.

Both MQ consumer overloads translate `NackRequeue` and callback exceptions to `reject(deliveryTag, AMQP::requeue)`. The library parameter is a bitmask; passing a boolean does not request requeue. `NackDiscard` uses zero flags. Push must requeue on Unacked persistence failure and deliver only after durable persistence succeeds.

## Business notifications

Relationship emits friend-request and accepted-request notifications, and Conversation emits creation notifications through a bounded Push `PushBatch` RPC after the domain write commits. Auth metadata and trace are forwarded. These online notifications are best effort, with no new durable retry guarantee. A creation broadcast omits the creator's `self` member state; each recipient obtains its own state through Conversation APIs. Tests: FN-WS-02/03/04.

## Delivery ACK convergence

**Flow:** Client WebSocket `MSG_PUSH_ACK` -> Push validation -> Redis unacked removal -> asynchronous Message `UpdateReadAck` -> MySQL convergence.

- Entry/contracts: `proto/push/notify.proto` `NotifyMsgPushAck`; `push/source/push_server.h` `onClientNotify`; `proto/message/message_service.proto` `UpdateReadAck`; `message/source/message_server.h` `UpdateReadAck`.
- Trust: Push binds identity when `CLIENT_AUTH` verifies the JWT, then compares ACK body `user_id` and `device_id` with connection identity. The WS handler constructs server-side brpc auth metadata for Message.
- Stores: Redis unacked state is exact and per device/user sequence; MySQL `conversation_member.last_ack_seq` is a monotonic durable high-water mark. These are different semantics and owners.
- Async/retry: Redis removal precedes a fire-and-forget Message RPC. Failure after removal can leave MySQL stale; failure before removal can cause duplicate resend. Heartbeats read due unacked entries and resend them. Message's monotonic update is idempotent, but the current Push-to-Message ACK RPC has no durable retry path.
- Tests: `tests/func/message_test.go` covers direct `UpdateReadAck`; `tests/func/scenarios_test.go` covers unread convergence. Verify the WS ACK path separately when changing it.
- Invariants: Push owns live routes, resend state, cross-instance fanout, and client ACK ingestion; Message owns persistent ACK convergence; preserve per-device exact removal and monotonic durable convergence; never trust ACK body identity without connection-derived validation.

## Authentication and forwarded context

**Flow:** Identity registration/login/refresh -> JWT -> Gateway or Push verification -> server-derived context -> downstream brpc metadata.

- Entry/contracts: `proto/identity/identity_service.proto`; `identity/source/identity_server.h`; `common/auth/jwt_codec.hpp`; `common/auth/jwt_store.hpp`; `gateway/source/gateway_auth.hpp`; `push/source/push_server.h`.
- Key loading: Identity, Gateway, and Push each resolve the complete JWT JSON document from exactly one of `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` at process startup. Identity signs and verifies; Gateway and Push verify. The codec is not hot-reloaded, so a key-set or `current_kid` change requires a controlled rollout of all affected processes.
- Stores: Identity uses MySQL for users/devices and Redis for active refresh tokens, rotation/reuse detection, and revocation state.
- Trust: Gateway validates Bearer access tokens and revocation before deriving metadata. Push verifies WS `CLIENT_AUTH`, queries revocation once at admission, and binds claim identity only after a `kNotRevoked` result. Revoked tokens and unavailable revocation state are rejected before connection, route, presence, or resend side effects. Downstream handlers use `common/auth/auth_context.hpp`; service-to-service forwarding uses `common/auth/forward_auth.hpp` where required.
- Sync/retry: Login and refresh are synchronous; refresh rotation detects reuse. Push performs no revocation lookup per message or heartbeat. Its admission boundary fails closed on Redis errors, while the legacy `JwtStore::is_revoked` bool API retains fail-open compatibility for unchanged callers.
- Tests: `tests/bvt/auth_test.go`, `tests/func/identity_test.go`, `tests/func/auth_middleware_test.go`, `tests/func/security_test.go`, `tests/func/scenarios_test.go`, and `tests/func/ws_notify_test.go` (`FN-WS-09`).
- Invariants: only Identity issues/refreshes tokens; access and refresh token purposes remain distinct; downstream identity comes from verified claims and forwarded metadata, not request bodies; Push admission must resolve revocation before publishing any authenticated-session side effect.

## Runtime secrets

### Current

**Flow:** deployment environment or mounted secret file -> common resolver -> service startup -> dependency/auth client construction.

- `common/config/secret_resolver.hpp` owns an allowlist of logical credentials and their direct-environment/`_FILE` names. It rejects missing or conflicting sources, invalid values, symlinks, non-regular files, unexpected owners, and group/other-accessible modes.
- JWT (Identity, Gateway, Push), application MySQL (Conversation, Identity, Media, Message, Relationship), RabbitMQ (Transmite, Message, Push), SMTP (Identity), and S3 application credentials (Media) use the resolver.
- Resolution happens once at startup. Missing or unsafe input prevents the service from accepting traffic; there is no hot reload or tracked/default fallback.
- Real credential changes and production rotation require explicit human approval. Values and credential-derived fingerprints must never appear in logs or operational evidence.

### Proposed

Redis authentication, dynamic reload, automatic rotation, and additional credential classes are not implemented. They require their own scoped Issues and executable tests; do not infer them from the current resolver. See `docs/operations/runtime-secrets.md` for the exact current contract and rollback procedure.

## Media upload and download

**Flow:** Apply/init -> presigned MinIO upload -> complete -> MySQL metadata/quota -> authenticated download request -> presigned MinIO GET.

Ordinary PUT URLs use the standard S3 presigner with the headers returned to the client, without implicit SSE-C headers. `use_path_style` disables AWS virtual addressing for internal MinIO hostnames; internal object verification and public presigning can use different endpoints.

- Entry/contracts: Gateway Media routes; `proto/media/media_service.proto`; `media/source/media_server.h`; `media/source/upload_handler.hpp`; `media/source/multipart_handler.hpp`; `media/source/download_handler.hpp`.
- Stores: MinIO holds bytes; MySQL holds `media_file`, blob-ref/dedup, multipart, and per-user quota state; Redis coordinates cleanup/locks where implemented.
- Boundaries: clients upload/download directly with short-lived presigned URLs. Apply validates size/MIME/hash/quota and records pending metadata; complete verifies object existence/size, converges dedup/refcount/quota, and is idempotent for committed files.
- Authorization: Gateway requires authentication. The current download handler permits an authenticated caller with a committed `file_id`; it does not enforce owner or conversation membership, as confirmed by `tests/func/media_test.go`'s other-user case. Do not overstate this boundary as resource-level authorization.
- Async/retry: cleanup handles stale pending, quarantine, and unreferenced object paths; client retries must preserve file/upload identifiers and completion idempotency.
- Tests: `tests/bvt/media_test.go`, `tests/func/media_test.go`, `tests/func/concurrency_test.go`, `tests/func/scenarios_test.go`, `tests/perf/upload_test.go`, `tests/pkg/verify/minio.go`.
- Invariants: service processes metadata rather than normal file bytes; only committed objects are downloadable; MySQL metadata/quota and MinIO object state must converge; preserve dedup and completion idempotency.
- Deduplication shares the stored object, not the file identifier: repeated uploads receive distinct metadata references to the same bucket/object key. Functional checks must validate both the distinct references and shared bytes.

Multipart routes require the same JWT boundary as single uploads. `partNumber` and `uploadId` are included before SigV4 signing. Test content follows the MIME allowlist and uses non-final parts of at least 5 MiB; FN-MD-07 verifies the downloaded bytes, and FN-MD-21 verifies signature tampering is rejected.

Media `FileInfo.public_url` is additive field 6: it contains the canonical configured public prefix plus the committed object's key, and stays empty for private objects. Identity discovers Media and resolves stored avatar file IDs through authenticated `GetFileInfo`; it returns an empty avatar URL when resolution is unavailable. Media configuration is authoritative for the public prefix. FN-ID-08 checks an actual HTTP GET and persisted profile reads.

Speech recognition rejects empty/unaligned PCM16 input. Until an ASR backend is integrated, valid input returns an explicit unavailable error rather than empty success (FN-MD-17/18/23).

## Presence and typing

**Flow:** Push WebSocket lifecycle -> Redis presence/routes -> Presence aggregation/subscriptions -> Presence or Push notification -> WebSocket.

Push routes each per-device write pipeline by its Redis key and stores enum names (`ONLINE`/`OFFLINE`). Presence accepts those names and legacy numeric enum values; malformed states are ignored. Registration alone does not create an online connection. BVT-018 opens an authenticated socket before asserting ONLINE.

- Entry/contracts: `push/source/push_server.h`; `proto/presence/presence_service.proto`; `presence/source/presence_server.h`; `proto/push/notify.proto`.
- Ownership: Push owns connections and route binding, writes per-device online/offline/heartbeat TTL state, and emits lifecycle notifications. Presence aggregates Redis device state, manages subscription sets and typing TTLs, and calls Push for delivery.
- Async/retry: lifecycle and typing notifications are fail-soft asynchronous Push RPCs; Redis TTL supplies eventual offline behavior; multi-instance fanout uses Push routing and cross-instance RPC.
- Tests: `tests/bvt/presence_test.go`, `tests/func/presence_test.go`, `tests/func/ws_notify_test.go`, `tests/pkg/fixture/ws.go`.
- Invariants: authenticated WS lifecycle drives presence; Redis is coordination state rather than durable user data; typing is ephemeral; route/presence TTL refresh and disconnect cleanup must remain aligned.

## Architecture-change synchronization

Any change to these paths, ownership boundaries, stores, protocols, topology, ordering, retry, idempotency, trust, or failure semantics must update this reference and any affected `technology-stack.md` or `repository-map.md` content in the same PR.

## Disposable CI startup

CI builds the nine native services once in the pinned Ubuntu builder, restores that artifact into each fresh test checkout, generates synthetic credentials, and runs Compose initialization before semantic readiness. MySQL, Redis Cluster, RabbitMQ, and MinIO initialization must converge before application services and runtime tests proceed. Existing environment files or persisted data cause test bootstrap to fail closed. Runtime results must be reported separately from static contracts.

Reliability RL-05 runs in its own disposable stack with Transmite user/session limits of 8/40 per minute. Its bounded outage burst exercises local fallback without depending on exhausting production defaults (600/3000). Push outage tests use the authenticated device ID and prohibit delivery of the particular unpersisted marker, while allowing redelivery of older durable messages. Multi-device login and caller-only deletion semantics remain unchanged.

The Redis fault pauses processes without withdrawing container DNS. Its recovery evidence does not cover stop/recreate, resolver failure or topology changes; those remain separate from the tested circuit behavior.
