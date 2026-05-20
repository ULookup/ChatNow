# Design: Auth Attachment Chain Optimization

**Date:** 2026-05-19
**Status:** Draft
**Parent:** [brpc-auth-attachment-design.md](./2026-05-19-brpc-auth-attachment-design.md)

## Motivation

Code review of `feat/brpc-auth-attachment` found 3 HIGH issues in the RpcMetadata lifecycle:

1. Gateway creates RpcMetadata twice per request (wasteful); `gateway_setup_trace` writes
   auth fields it shouldn't own (leaky separation).
2. PushToUser/PushBatch call `extract_auth` on empty attachments from MQ-driven paths,
   wasting CPU with no business purpose.
3. `onClientNotify` constructs RpcMetadata without `trace_id`, leaving the ACK path
   untraceable.

These are correctness/clarity problems, not bugs. Fix now before they ossify.

## Decision

### 1. Gateway: Clean Separation of Trace and Auth

**Before (broken):**
```
handle_request():
  RpcMetadata meta;                              // creation #1
  gateway_setup_trace(req, meta)                 // writes trace_id → meta
  extract trace_id → discard meta                // wasted

forward():
  RpcMetadata meta;                              // creation #2
  gateway_setup_trace(req, meta, uid, did)       // writes trace_id + user_id + device_id
  apply_auth_to_brpc(meta, auth)                 // writes user_id + device_id again
  apply_metadata_to_brpc(meta, cntl)             // serialize
```

**After:**
```
handle_request():
  trace_id = resolve_trace_id(req)               // no meta needed
  → set X-Trace-Id response header
  → handler (→ forward())

forward():
  RpcMetadata meta;                              // single creation
  gateway_setup_trace(req, meta)                 // writes trace_id + LogContext ONLY
  apply_auth_to_brpc(meta, auth)                 // writes user_id + device_id + jwt_jti ONLY
  apply_metadata_to_brpc(meta, cntl)             // serialize
```

**Changes:**

- `gateway_trace.hpp`: Remove `user_id` and `device_id` parameters. Signature becomes `gateway_setup_trace(req, meta)`. Delete lines 44-49 (the `set_user_id`/`set_device_id` block). Calls `LogContext::set(trace_id, "", "")` — sets trace_id, leaves identity blank (filled later by auth middleware).

- `gateway_auth.hpp`: Append at end of `apply_auth_to_brpc`:
  ```cpp
  ::chatnow::log::LogContext::set(
      ::chatnow::log::LogContext::current().trace_id,
      a.user_id, a.device_id);
  ```
  This updates LogContext with real identity so that forward()'s LOG output carries user_id/device_id. (Dependency on `log/log_context.hpp` already satisfied via `gateway_trace.hpp`.)

- `gateway_server.h:handle_request()`: Replace the 4-line meta block (lines 187-190) with:
  ```cpp
  std::string trace_id = ::chatnow::gateway::resolve_trace_id(req);
  res.set_header("X-Trace-Id", trace_id);
  ```

- `gateway_server.h:forward()`: Update call site (lines 124-127):
  ```cpp
  ::chatnow::rpc::RpcMetadata meta;
  ::chatnow::gateway::gateway_setup_trace(httpreq, meta);
  ::chatnow::gateway::apply_auth_to_brpc(meta, auth);
  ::chatnow::gateway::apply_metadata_to_brpc(meta, cntl);
  ```

**Why this way:** Mirrors Envoy/gRPC interceptor chains — each middleware owns one concern. `gateway_setup_trace` = tracing middleware. `apply_auth_to_brpc` = auth middleware. The `RpcMetadata` proto object is the shared data plane they both append to.

### 2. Push: Remove extract_auth from PushToUser and PushBatch

`PushToUser` and `PushBatch` do not make authorization decisions. They use `request->user_id()`, `request->target_device_ids()`, and `request->notify()` for all business logic. The `extract_auth` call is pure waste — on MQ-driven paths the attachment is empty, causing a parse+throw+catch cycle per call.

**Change:** Delete the try/catch block that calls `extract_auth` in both handlers.

- `push_server.h:89-95` — PushToUser
- `push_server.h:156-158` — PushBatch

The comment at the PushBatch handler acknowledging "auth extraction failure doesn't block" is vestigial documentation of a non-requirement; remove it.

**Why remove rather than fix:** An MQ consumer has no inbound RPC context. Constructing an RpcMetadata from MQ message fields to simulate one is putting context where it doesn't belong — the message body already carries all needed data (user_id in InternalMessage, trace_id in notify_template). This follows the "message self-containment" principle used by Kafka, RabbitMQ, SQS, and WeChat's WQueue.

### 3. Push: Generate trace_id in onClientNotify for WS ACK path

`onClientNotify` (WS handler) constructs RpcMetadata for an `UpdateReadAck` RPC but omits `trace_id`. Since this is a new entry point (client-initiated, analogous to an HTTP request), it should seed a new trace.

**Change:** In `push_server.h:348-354`, after constructing meta, add:
```cpp
meta.set_trace_id(::chatnow::utils::gen_trace_id());
```

This follows the pattern used by WhatsApp/Telegram/Discord — client ACKs get a new server-side trace span rather than attempting to propagate a nonexistent upstream trace.

### 4. Auth: Improve extract_auth Failure Logging

When `extract_auth` fails, the current message has no context for debugging.

**Change:** In `auth_context.hpp:39`, include the attachment size:
```cpp
LOG_WARN("Failed to parse RpcMetadata from attachment, size={}",
         cntl ? cntl->request_attachment().size() : 0);
```

For the null-controller case (`!cntl`), the size=0 default makes it self-evident.

## Files Changed

| File | Change |
|------|--------|
| `gateway/source/gateway_trace.hpp` | Remove `user_id`/`device_id` params; function becomes trace-only |
| `gateway/source/gateway_auth.hpp` | Append `LogContext::set` call so forward() logs carry user_id/device_id |
| `gateway/source/gateway_server.h` | `handle_request()`: delete meta block, use `resolve_trace_id` directly. `forward()`: update call sites |
| `push/source/push_server.h` | Delete `extract_auth` in PushToUser (L89-95) and PushBatch (L156-158); add `gen_trace_id()` in onClientNotify |
| `common/auth/auth_context.hpp` | Improve parse-failure log message |

## Non-Goals

- IOBuf zero-copy optimization in `extract_auth` — RpcMetadata is &lt;200 bytes, `to_string()` cost is negligible (premature optimization)
- Standardizing attachment write strategy (`append` vs `=`) — current behavior is correct in each context; no bug to fix
- Adding `forward_auth_metadata` to MQ-to-PushBatch path — MQ consumers have no inbound RPC context by design
