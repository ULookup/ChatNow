---
name: chatnow-orienting
description: Use when entering ChatNow for the first time, answering architecture questions, planning cross-service work, or changing service boundaries, infrastructure topology, or core flows
---

# Orienting in ChatNow

## Overview

Build an architecture assessment from the target version's executable evidence. Preserve ownership, trust, ordering, idempotency, and failure invariants before proposing a change.

## Evidence precedence

Use evidence in this order:

1. Source and Protobuf contracts.
2. Executable configuration, CMake, and Docker Compose.
3. Tests and CI workflows.
4. Git history for intent and version-specific context.
5. README, design documents, roadmaps, and changelogs as secondary context only.

Verify secondary claims against higher-precedence evidence. Cite repository paths and symbols or lines for every material conclusion.

## Orientation workflow

1. Resolve the primary Issue, its architecture/core-flow acceptance, target version line, current branch, and exact commit. Inspect the target version rather than assuming the checkout is correct.
2. Read the relevant service entry points, Protobuf contracts, configuration, CMake, Compose, tests, and history.
3. Trace entry points and calls across HTTP, brpc, RabbitMQ, WebSocket, Redis, MySQL, Elasticsearch, MinIO, and etcd as applicable.
4. Map state ownership, trusted identity derivation, sync/async boundaries, retries, idempotency keys, outboxes, and recovery behavior.
5. State current invariants before evaluating a proposal. Distinguish durable sources of truth from caches, routing state, and acceleration layers.
6. Separate **Current behavior** from **Proposed behavior**. Never describe an intended design as implemented.
7. Identify compatibility, partial-failure, observability, deployment, and test impact.
8. For any architecture, service-boundary, infrastructure-topology, or core-flow change, require updates to this Skill's affected references in the same PR. A follow-up Issue is not a substitute.

Stop and request resolution when the target version is ambiguous, executable sources contradict each other, or the primary architecture Issue/acceptance is missing.

## Load references conditionally

- Read [technology-stack.md](references/technology-stack.md) for build, dependency, configuration, runtime, infrastructure, or verification questions.
- Read [repository-map.md](references/repository-map.md) when locating ownership, ports, services, stores, or first-read files.
- Read [core-flows.md](references/core-flows.md) for HTTP, message, ACK, authentication, media, presence, ordering, retry, or delivery changes.
- On first entry or cross-service architecture work, read all three.

## Required output

Return these fields in order:

1. **Target version** — Issue, target version line, branch, and commit.
2. **Evidence** — executable paths/symbols inspected and any secondary sources used.
3. **Affected services** — services, contracts, stores, trust boundaries, and sync/async boundaries.
4. **Current flow** — verified end-to-end behavior and ownership.
5. **Changed invariants** — proposed behavior separately, including ownership, ordering, idempotency, and source-of-truth changes.
6. **Failure/compatibility impact** — retries, partial failures, rollout, protocol/config/deployment compatibility, observability, and tests.
7. **Skill updates** — exact affected Skill/reference files that must change in the same PR, or `None` with evidence that architecture and core flows are unchanged.

## Common mistakes

| Mistake | Correction |
|---|---|
| Starting from a roadmap or README | Trace the executable path first, then reconcile prose. |
| Naming services but not state owners | Name each store, cache, queue, writer, reader, and durable source of truth. |
| Calling an MQ or WebSocket path exactly-once | Assume at-least-once delivery and identify idempotent effects. |
| Treating a client field as trusted identity | Trace where Gateway or Push derives identity and how brpc metadata is built. |
| Blending current and proposed topology | Use separate current and proposed sections. |
| Deferring architecture references | List same-PR Skill/reference updates explicitly. |
