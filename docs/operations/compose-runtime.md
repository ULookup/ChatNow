# Compose Runtime Operations

Target version: `3.0-dev`
Status: Unverified
Reviewed: 2026-09-13

This is the canonical operating contract for the repository-root Compose runtime. Issue #88 has exercised cold startup, all nine native service builds, and BVT on Linux CI. Functional and Reliability have exposed unresolved failures; this profile is not a validated release deployment. PR #89 records the exact tested commits and individual gate results. Always obtain fresh evidence for the intended release pair.

The root profile is a local-development runtime intended for disposable environments. It is not the production HA topology tracked by Issue #73. Issue #88 integrates CI using fresh runner checkouts, shared native service artifacts, and synthetic credentials. Existing environment files and persistent data are never overwritten by the bootstrap helper.

## Inputs and scope

The root `docker-compose.yml` requires synthetic local values for the bootstrap and application secret names it references. Supply them through an ignored local environment file or another approved runtime source. Never commit values or copy production credentials into this profile. The authoritative variable inventory and handling rules are in [Runtime Secret Management](runtime-secrets.md).

The root profile owns the integrated application topology. Do not combine it with `docker/docker-compose.yml`; that supplemental file is not part of the root runtime contract.

## Startup and convergence

The intended operator sequence is:

```bash
docker compose config
docker compose up -d --build
./scripts/wait_for_services.sh
```

These commands describe the implemented interface, not a recorded successful cold start. `docker compose config` validates interpolation and topology only. `scripts/wait_for_services.sh` is the cross-stack readiness gate; container creation or a successful TCP connection alone is insufficient.

The root topology uses bounded health checks and one-shot convergence services:

- `mysql-init` waits for MySQL health, runs the checksum-tracked SQL migrator, converges application users and grants, then exits. Application services that use MySQL wait for `service_completed_successfully`.
- `redis-cluster-init` waits for all six Redis nodes, creates the three-master/three-replica cluster only when no healthy cluster metadata exists, verifies all 16,384 slots, then exits. It refuses to recreate degraded existing metadata. Every node advertises its stable Compose service hostname so persisted cluster metadata does not retain ephemeral container IPs.
- `rabbitmq-init` waits for RabbitMQ health, converges the Transmite, Message, and Push application identities and their scoped permissions through the Management API, then exits. RabbitMQ uses a fixed hostname and node name so its persisted Mnesia path remains stable.
- `minio-init` waits for MinIO health, converges the public and private media buckets, anonymous-access policies, and the Media application identity, then exits.
- Application containers depend on the relevant infrastructure health checks and one-shot initializers. Their shared `entrypoint.sh` performs bounded TCP dependency polling; it is not the semantic full-stack readiness gate.

Every initializer fails on missing required inputs and must remain non-resident. The Redis, RabbitMQ, and MinIO initializers also fail when their bounded attempt limits are exhausted. Fixed startup sleeps and `tail -f` sentinels are not readiness mechanisms.

## MySQL bootstrap and migrations

`mysql-init` mounts `sql/` read-only and executes `scripts/init_mysql.sh`. The runner creates `chatnow.schema_migrations`, computes a SHA-256 checksum for every ordered `V*.sql` file, skips an already-applied version only when its checksum matches, and fails closed if an applied migration was changed. New schema changes require a new versioned file; never edit an applied migration.

The current ODB object set contains 17 tables:

```text
conversation
conversation_member
friend_apply
media_blob_ref
media_file
media_user_quota
message
message_attachment
message_mention
message_pin
message_reaction
message_read
relation
user
user_block
user_device
user_timeline
```

`sql/V1__core.sql` owns the 14 non-media tables and `sql/V4__media.sql` owns the three media tables. Their table creation is idempotent. After schema convergence, `scripts/converge_mysql_users.sh` creates or updates five application identities (`chatnow_identity`, `chatnow_conversation`, `chatnow_relationship`, `chatnow_message`, and `chatnow_media`) and reapplies their table-scoped grants.

This is a small forward-only repository migrator, not a general rollback engine. MySQL DDL is not treated as transactional across a whole migration and the migration record insertion. Preserve a database backup before operating on non-disposable data, stop on checksum mismatch or partial DDL, and resolve the state explicitly rather than deleting the migration ledger.

## Media and MinIO endpoints

Media has two endpoint roles:

| Role | Local process configuration | Root Compose container configuration | Owner |
|---|---|---|---|
| Internal S3 operations | `http://127.0.0.1:19000` | `http://minio:9000` | Media process to MinIO |
| Client-facing presigned URLs | `http://127.0.0.1:19000` | `http://127.0.0.1:19000` | Test or local client to published MinIO S3 port |

`conf/media.json` is the local-process configuration. Root Compose mounts `conf/docker/media.json` read-only into Media. `S3Client` uses the internal endpoint for bucket and object operations and a separate presign client for generated URLs. The public endpoint must be reachable by the actual client; the loopback value is valid only for clients running on the Compose host and must be replaced by deployment-specific configuration elsewhere.

Root Compose publishes MinIO S3 on `127.0.0.1:19000` and its console on `127.0.0.1:19001`, leaving Gateway HTTP on `9000` and Push WebSocket on `9001`.

## RabbitMQ host contract

`mq_host` is a host name or address only. Transmite, Message, and Push builders append the fixed AMQP port `5672` when constructing the URL and percent-encode userinfo credentials. Container configuration therefore uses `rabbitmq`; local configuration uses `127.0.0.1`. Supplying `host:5672` produces an invalid double-port URL and is rejected.

## Semantic readiness

`scripts/wait_for_services.sh` uses one bounded deadline and polls the following observable conditions:

- Redis reports `cluster_state:ok`, all 16,384 slots assigned and healthy, and exactly six known nodes.
- MySQL contains all 17 ODB tables and the five required application users.
- RabbitMQ is running without local alarms and all three scoped application identities have permissions.
- Elasticsearch reaches yellow or green cluster health.
- MinIO reports ready and both required buckets are addressable.
- etcd contains exactly the eight expected service registration keys for Identity, Media, Transmite, Message, Relationship, Conversation, Presence, and Push.
- unauthenticated Gateway `GET /health` returns success.
- the Push WebSocket listener is reachable on host port `9001`.

Gateway `/health` is dependency-aware, not a process-liveness response. It returns HTTP `200` only when the Gateway service manager currently has at least one discovered channel for each of the eight business services; it returns HTTP `503` otherwise. It does not replace the stateful infrastructure probes above.

Channel presence does not prove RPC reachability after container IP changes. The current brpc channel cache can retain an old IP for a reused service hostname. Redis Cluster metadata can also retain old peer IPs after a simultaneous container restart. Do not treat a successful cold start as rolling-update or restart-recovery evidence. Keep persisted data and investigate stale discovery/cluster addresses before attempting recovery; never recreate an existing cluster merely to make readiness pass.

The Push check is currently bounded TCP reachability because Push has no separate semantic health endpoint. Do not describe that individual probe as end-to-end WebSocket delivery evidence.

## Shutdown and clean-state boundary

Stop the profile with:

```bash
docker compose down
```

The root profile persists infrastructure under `middle/data` through bind mounts. `docker compose down -v` does not remove bind-mounted state and therefore does not prove a clean-slate restart. Do not delete a shared `middle/data` tree or claim repeatable cold-start behavior without an explicitly disposable path, the separate CI storage override, and fresh dynamic evidence.

## Evidence required before a readiness claim

Static Compose contracts, shell syntax, formatting, compilation, and `docker compose config` are supporting checks only. A full runtime claim requires fresh evidence for the exact commit from an empty disposable state, successful `scripts/wait_for_services.sh`, and the applicable Go gates. Report each gate as passed, failed, blocked, or not run from its actual result. Any applicable failure keeps the release verdict `Unverified`.
