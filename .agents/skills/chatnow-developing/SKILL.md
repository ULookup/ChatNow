---
name: chatnow-developing
description: Use when changing ChatNow production C++, Protobuf contracts, configuration, databases, caches, message queues, service discovery, or runtime infrastructure
---

# Develop ChatNow Services

## Core principle

Preserve ownership, trust, failure, and repeated-execution invariants with the smallest Issue-scoped change. A successful response is valid only when every required dependency and asynchronous handoff has defined bounded behavior.

## Entry gates

1. Confirm a valid primary Issue defines the target version, scope, acceptance criteria, risks, architecture/core-flow impact, and required Skill updates. Stop before implementation if it does not.
2. Use `chatnow-orienting` to verify affected owners, trust boundaries, durable sources of truth, caches, calls, queues, stores, and current invariants from executable evidence.
3. Use `chatnow-testing` before production code. Record a pure-Go behavioral test that failed for the expected missing behavior. If production code predates the observed RED, delete it and restart test-first.
4. Find the nearest repository pattern in the same subsystem and version line. Cite the files and preserve their contracts unless the Issue explicitly changes them.

## Implementation workflow

1. State the invariant being changed and the invariants that must remain true.
2. Assign ownership for identity, durable state, cached or coordination state, transactions, asynchronous work, and cleanup. Derive authority from authenticated server context, never an untrusted client field.
3. Specify success, synchronous failure, asynchronous failure, timeout, cancellation, retry, duplicate execution, partial completion, and recovery before editing code.
4. Make the minimum diff required by the observed RED. Do not mix unrelated refactors, formatting, generated output, or speculative abstractions.
5. Apply every relevant boundary rule below.
6. Reach GREEN with the target Go test, run relevant same-layer regressions, then refactor only while green. Report actual commands and results; do not invent unavailable commands or runtime evidence.
7. Recheck compatibility, observability, deployment/configuration, cleanup, and same-PR Skill synchronization.

## Boundary rules

### RPC and asynchronous callbacks

- Preserve server-derived authentication and authorization context across the brpc attachment; validate required metadata at the receiver.
- Map transport, timeout, authentication, validation, dependency, and application failures deliberately. Do not turn every exception into success.
- Set a bounded timeout and define whether a timed-out operation is safe to retry. Make retry limits and backoff explicit.
- Guarantee the brpc closure runs exactly once on every path. Keep response, controller, callback captures, channels, and dependent objects alive until the final callback; prevent double completion and use-after-free.

### RabbitMQ

- Identify the exchange type/name, routing key, queue owner, binding, durability, publisher-only or consumer role, and deployment configuration. Do not declare an orphan or conflicting topology.
- Define publisher-confirm handling for acknowledged, rejected, lost/unknown, synchronous-error, and timeout outcomes. Return success only after the contract's success boundary.
- Treat delivery as at-least-once. Define retry ownership, bounded retry/backoff, redelivery behavior, idempotency key/effect, duplicate result, and any Outbox or reaper recovery path.
- Preserve trace headers and ensure consumers acknowledge only after the owned durable effect reaches its declared boundary.

### Redis

- Classify each key as cache, coordination/lease, idempotency state, sequence state, routing/presence state, or authoritative security state. Name the durable source of truth when Redis is not authoritative.
- Choose fail-open, fail-closed, or explicit degraded behavior from that classification. Authorization, revocation, uniqueness, sequencing, and destructive decisions must not silently use cache-miss or exception as success.
- Bound each lookup or lock acquisition by timeout and cancellation behavior. Define retry limits, fallback, stale-data policy, TTL, key ownership, and cleanup.
- Distinguish miss, confirmed absence, timeout, connection failure, parse failure, and partial write in code, logs, responses, and tests.

### State, concurrency, and resources

- Define the database transaction boundary, commit point, rollback behavior, and relationship between database writes and external side effects. Never imply atomicity across MySQL, Redis, Elasticsearch, RabbitMQ, or RPC without an implemented protocol.
- For repeated execution, state the idempotency key, uniqueness or monotonic guard, result after prior success, behavior after partial failure, and retry owner.
- Protect shared state with the nearest established synchronization pattern. State lock scope/order, contention and timeout behavior, and object lifetime across threads, event loops, goroutines in tests, callbacks, shutdown, and cancellation.
- Assign one owner to close or release every lock, socket, channel, Redis lease/key, database handle, callback, thread, and temporary test resource on all paths.

## Compatibility and observability

- Preserve Protobuf field numbers, wire meanings, response envelopes, public error text, configuration defaults, exchange/queue contracts, database compatibility, and rolling mixed-version behavior unless the Issue explicitly authorizes a compatibility change.
- Treat intentional public compatibility or settled-semantic changes as requiring human approval.
- Use English for new identifiers, comments, and structured log messages. Include safe request/trace and outcome context; never log credentials, tokens, full message content, or unnecessary personal data.
- Update affected ChatNow Skills and references in the same PR when architecture, service boundaries, infrastructure topology, core flows, or test architecture change. A follow-up Issue is not a substitute.

## Required output

Return these fields in order:

1. **Changed invariants** — the Issue-scoped behavior change and preserved ownership, trust, ordering, and source-of-truth rules.
2. **Files/ownership** — minimal changed files, nearest patterns used, and owners of state, side effects, callbacks, and cleanup.
3. **Failure behavior** — bounded synchronous, asynchronous, timeout, cancellation, partial-failure, and recovery semantics.
4. **Retry/idempotency** — retry owner, limits/backoff, idempotency key or guard, duplicate result, and repeated-execution behavior.
5. **Compatibility** — wire, error, configuration, topology, data, rollout, and public-behavior impact.
6. **Tests** — selected Go layer/case ID, exact observed RED, GREEN and regression commands/results, cleanup ownership, and checks not run.
7. **Skill updates** — exact Skill/reference files changed in the same PR, or `None` with evidence that no governed architecture, core-flow, infrastructure, or test rule changed.

## Common mistakes

| Mistake | Required correction |
|---|---|
| Returning success for every Redis exception | Classify the key and failure, select an explicit safe mode, bound the call, and test each outcome. |
| Retrying an unknown MQ publish as a new effect | Use publisher confirms and an idempotency guard; define the duplicate result and retry owner. |
| Completing an RPC before its callback or capture lifetime ends | Keep dependencies alive and use an exactly-once completion guard. |
| Updating a database and broker as if one transaction | Define the commit boundary and use the established Outbox/recovery pattern where required. |
| Copying a nearby pattern without checking trust or ownership | Cite the pattern, then verify its boundary matches this operation. |
| Calling a compile or static check a behavior test | Report only an observed Go behavioral RED/GREEN as runtime evidence. |
