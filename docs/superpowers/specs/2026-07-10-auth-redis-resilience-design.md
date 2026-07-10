# Auth and Redis Resilience Design

## Scope

Address GitHub issues #21, #22, and #48 on top of `3.0-dev`. The work is
limited to authentication, WebSocket admission, and Redis-backed rate-limit
and idempotency paths. It does not add periodic validation to already-open
WebSocket connections or change token formats.

## Goals

- A revoked access token must not establish a Push WebSocket connection.
- Repository source must not contain runnable MySQL or SMTP credentials.
- A Redis outage must not hold every request until the Redis client timeout,
  disable rate limiting, or amplify traffic toward downstream services.
- The healthy Redis path remains the distributed source of truth and retains
  its existing atomic semantics.

## Design

### WebSocket revocation check (#21)

`PushServerBuilder` will construct and retain a `JwtStore` from the same
Redis client used by Push. `JwtStore` will expose revoked, not-revoked, and
unavailable outcomes rather than representing an outage as `false`.
`PushServiceImpl::_handle_client_auth_` will verify the JWT, then query the
token JTI before inserting the connection or writing presence. A revoked JTI
or unavailable store closes the connection. Gateway access-token validation
and Identity refresh/replay validation use the same fail-closed outcome; a
store write needed to preserve refresh replay protection also fails closed.
This prevents a logout token from re-establishing a long-lived connection or
an outage from reviving credentials, without adding a Redis operation to the
per-message or per-push hot path.

### Secret configuration (#22)

`mysql_pswd` and `mail_paswd` defaults become empty. At process start, values
may be supplied through gflags (including their normal environment/config
injection mechanism), but startup validates that both are non-empty before
creating the MySQL or SMTP client. The checked-in local and Docker examples
use placeholders and explicitly instruct operators to inject secrets. The
deployment note requires rotation of credentials that were committed
previously.

### Redis circuit breaker and degradation (#48)

Introduce a small, process-local Redis health gate shared by call sites in a
service. It has `closed`, `open`, and one-probe `half-open` states. A Redis
transport failure opens the gate; while open, calls fail immediately without
touching the pool. After a short monotonic cooldown, exactly one request may
probe Redis; success closes the gate and failure renews the cooldown. State
uses atomics and steady-clock timestamps, so it is non-blocking and safe for
concurrent request threads. Redis connection, socket, and pool-wait timeouts
become bounded configuration values suitable for the service latency budget;
the implementation must not retain a hard-coded two-second outage delay.

Call-site policy is explicit:

| Path | Redis healthy | Redis unavailable or gate open |
| --- | --- | --- |
| JWT access/refresh revocation and replay protection | Redis state lookup/write | deny authentication or refresh |
| Distributed rate limit | existing Redis limiter | local sharded token bucket, deny when exhausted |
| Message idempotency | existing SET NX protocol | proceed without Redis; retain database uniqueness fallback |

The local limiter uses a bounded, striped map keyed by the existing limiter
identity and monotonic refill. It is only a conservative outage fallback;
it is not intended to synchronize limits across instances. Stripes avoid a
global mutex on the request hot path and capacity eviction bounds memory.

## Failure handling and observability

- A Redis exception records a failure once per breaker transition, rather
  than logging every rejected request.
- Gate-open rejections and local limiter decisions are observable through
  metrics or existing counters, with no token/JTI values in logs.
- Authentication failure messages remain generic, so an unavailable Redis
  backend is not distinguishable to clients.

## Tests

- Push auth: revoked JTI and unavailable `JwtStore` both reject before
  connection registration or presence writes; a valid JTI is admitted.
- Gateway access and Identity refresh: unavailable Redis state rejects the
  credential rather than treating it as not revoked.
- Config: the production defaults are empty and missing required secrets
  fail before service construction.
- Breaker: concurrent callers short-circuit after an initial failure;
  cooldown permits only one recovery probe; successful recovery restores
  Redis operations; timeout configuration bounds the failure latency.
- Rate limiter: a Redis outage still enforces the local configured budget;
  stripe capacity bounds retained keys.
- Idempotency: gate-open behavior preserves the existing fail-open,
  database-backed fallback without issuing Redis calls.

## Non-goals

- Retroactively rotating deployed credentials; operators must perform that
  deployment action.
- Per-message Redis checks for established WebSocket connections.
- Cross-instance exact rate-limit accounting while Redis is unavailable.
