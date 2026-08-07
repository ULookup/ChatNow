---
name: chatnow-securing-changes
description: Use when ChatNow work touches authentication, authorization, user input, personal data, SQL or search queries, media or file paths, networking, credentials, logs, production, or irreversible data operations
---

# Secure ChatNow Changes

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

## Core principle

Treat every external value as untrusted until a named server boundary validates it. Minimize authority and exposed data, and fail securely when a high-impact decision cannot be made safely.

## Required inputs

Before editing, identify:

- the primary Issue, target version, acceptance criteria, and compatibility constraints;
- each external actor, server boundary, credential, personal-data field, input channel, query, object key, path, outbound destination, and privileged operation;
- the server-owned source of identity and authorization, the durable data owner, and the least-privileged component allowed to perform each effect;
- the pure-Go test layer and case ID selected with `chatnow-testing`.

Stop when identity or ownership is ambiguous, authorization cannot be evaluated from server-verified context, safe input constraints are unknown, or required human approval is absent.

## Mandatory workflow

1. Draw the trust path from the external input through Gateway, brpc metadata, queues, services, and stores. Label where authentication, authorization, validation, redaction, and privilege changes occur.
2. Derive actor identity only from verified server authentication context. Treat client-provided `user_id`, owner, role, tenant, route, or permission fields as untrusted targets or claims; never use them as authority.
3. Authorize the exact resource and action at the service that owns the effect. Check relationship, membership, block, ownership, and scope rules against authoritative state. Do not infer authorization from authentication alone.
4. Constrain inputs before side effects: type, length, range, encoding, enum or allowlist, cardinality, pagination, resource limits, and destination. Reject ambiguous or malformed input.
5. Apply the boundary-specific controls below with least-privileged credentials, network access, service roles, storage permissions, and data access.
6. Define secure behavior for missing, stale, timed-out, contradictory, or unavailable security state. Authentication, authorization, credential, ownership, destructive, and privacy decisions fail closed; return a bounded safe error and emit only non-sensitive diagnostics.
7. Use `chatnow-testing` to observe a failing pure-Go adversarial or regression test before production code, then reach GREEN and run the relevant same-layer regressions.
8. Recheck compatibility, rollout, logging, retention, cleanup, and approval boundaries before completion.

## Boundary controls

### Identity, credentials, and logs

- Preserve server-derived identity across trusted metadata and validate it again at the receiving boundary. Never forward a client identity as authenticated context.
- Treat `docs/operations/runtime-secrets.md` as the canonical credential inventory and runtime contract. Reinspect `common/config/secret_resolver.hpp` and the executable consumer before extending it; do not create a parallel loader.
- For a migrated credential, accept exactly one of its allowlisted direct environment variable or `_FILE` companion. Reject a direct/file conflict, missing required input, an empty value, an unreadable file, a symlink, non-regular input, unexpected ownership, or permissions that grant access beyond the intended runtime identity. Do not fall back to tracked configuration or a compiled default.
- Keep deployment bootstrap credentials separate from least-privileged application credentials. Local and CI values must be unique synthetic fixtures; never copy a real credential into a repository file, command line, workflow output, test failure, or artifact.
- Never log or expose bearer tokens, authorization headers, passwords, signing keys, session secrets, cookies, presigned URLs, or real credentials. Do not create, log, or expose any credential-derived token fingerprint, including a hash, keyed HMAC, prefix, suffix, encoded value, or truncated derivative. Permit such a derivative only when an approved protocol explicitly requires it, constrain it to that protocol, and never repurpose it for diagnostics; prefer request or trace IDs.
- Minimize personal data. Prefer a trace/request ID or purpose-specific opaque correlation ID. Redact or omit user identifiers, device identifiers, message content, contact data, object names, and search text unless the Issue documents necessity, access, retention, and a safe representation.
- Write English structured logs with stable event and outcome fields. Avoid free-form concatenation of untrusted values and log injection; encode fields through the established logger.

### SQL and Elasticsearch

- Bind every SQL value through ODB or parameterized statements. Never concatenate, interpolate, or escape user input into SQL. Select dynamic identifiers or sort directions only from server-owned allowlists.
- Build Elasticsearch requests with the structured JSON/query DSL API. Allowlist searchable fields, operators, sort keys, analyzers, and result limits. Keep user text in value nodes; never splice it into query JSON, scripts, field names, index names, or raw query-string syntax.
- Bound query complexity, pagination, timeouts, and returned fields. Preserve authorization filters on every search path; a search hit does not grant access.

### Media, object keys, paths, and networking

- Construct object keys from server-owned prefixes and validated identifiers. Do not accept a client-supplied bucket, namespace, absolute key, or ownership prefix.
- Parse and normalize paths once, reject absolute paths, traversal segments, encoded traversal, separators in single-segment identifiers, NUL bytes, and symlink escapes, then verify the resolved target remains under the intended root before access.
- Allowlist outbound schemes, hosts, ports, redirects, and resolved address classes where destinations are influenced by input. Block loopback, link-local, private, metadata, and internal service targets unless the operation explicitly requires and authorizes them.
- Bound upload, download, decompression, body, redirect, retry, and timeout limits. Validate content from bytes rather than trusting names or client media types.

## Human approval boundaries

Obtain explicit human approval before using, rotating, revoking, or changing real credentials; operating in production; performing irreversible migration or deletion; or intentionally changing public compatibility or settled product semantics. Approval must name the exact operation and scope. A deadline, temporary diagnostic, rollback plan, or existing access does not substitute for approval.

## Test contract

Add pure-Go adversarial and regression cases for every changed boundary. Include applicable cases for:

- missing, invalid, expired, revoked, mismatched, and replayed credentials;
- spoofed client identity and cross-user, cross-conversation, blocked-user, non-member, and ownership violations;
- SQL metacharacters and Elasticsearch field/operator/script/query-string injection, authorization-filter bypass, excessive limits, and expensive queries;
- `..`, absolute, mixed-separator, percent-encoded, NUL, symlink, bucket/prefix, and cross-user object-key traversal;
- secret and personal-data absence from logs, responses, traces, fixtures, failure output, and generated artifacts;
- direct/file secret conflicts, missing/empty input, unsafe file ownership or permissions, symlinks, and proof that tracked/default values cannot silently take over;
- dependency timeout/unavailability at a high-impact decision, proving a bounded secure failure with no partial privileged effect.

Use unique synthetic identities and credentials only. Assign cleanup ownership for users, rows, indexes/documents, objects, keys, sockets, and temporary files.

## Required output

Return these fields in order:

1. **Trust boundaries**: actors, inputs, identity derivation, validation points, privilege transitions, and authoritative owners.
2. **Threats and controls**: abuse paths and the exact authorization, constraint, query, path, network, redaction, and least-privilege controls.
3. **Failure behavior**: timeout, unavailable-state, partial-effect, retry, cleanup, and fail-closed semantics.
4. **Data and logs**: credentials and personal data touched, minimization/redaction, English structured fields, access, and retention.
5. **Tests**: Go layer/case ID, observed RED, GREEN/regression results, adversarial coverage, cleanup, and checks not run.
6. **Approvals and compatibility**: required human approvals and public, wire, data, rollout, or settled-semantic impact.

## Common mistakes

| Rationalization | Required correction |
|---|---|
| "Log the token temporarily; delete it tomorrow." | Never record a reusable credential. Correlate with a safe request or opaque purpose-specific ID. |
| "A hash, HMAC, or truncated token is safe to log." | It remains credential-derived. Prohibit it unless an approved protocol explicitly requires it; prefer request or trace IDs. |
| "The client already knows its user ID." | Knowledge is not authority; derive the actor from verified server context. |
| "Escaping makes this SQL or JSON safe." | Bind SQL values and construct allowlisted Elasticsearch DSL nodes. |
| "The storage SDK normalizes the key." | Constrain server-owned keys and prove containment before access. |
| "Fail open to preserve availability." | High-impact uncertainty fails securely; define a bounded safe error. |
| "We can approve after the production diagnostic." | Obtain explicit approval before real credentials, production, irreversible data, or intentional compatibility changes. |
