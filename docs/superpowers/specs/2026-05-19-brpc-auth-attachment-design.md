# Design: Migrate Auth Metadata to brpc Attachment

**Date:** 2026-05-19
**Status:** Draft

## Problem

Current auth metadata (`x-user-id`, `x-device-id`, `x-trace-id`, `x-jwt-jti`) is passed via
`cntl->http_request().SetHeader()` / `GetHeader()`. This does not work correctly over
baidu_std protocol — the HTTP header interface on brpc Controller is a compatibility shim
whose serialization behavior is unreliable under baidu_std.

## Decision

Use brpc's native `cntl->request_attachment()` with Protobuf serialization to carry all
RPC metadata. This is the baidu_std protocol's intended mechanism for side-channel data,
analogous to gRPC Metadata.

## 1. RpcMetadata Proto

New file: `common/auth/metadata.proto`

```protobuf
syntax = "proto3";

package chatnow.rpc;

message RpcMetadata {
    string trace_id  = 1;
    string user_id   = 2;
    string device_id = 3;
    string jwt_jti   = 4;
}
```

`common/auth/metadata_keys.hpp` will be removed. The four header key constants are no
longer needed — field access is through the generated proto class.

## 2. Gateway Write Path

Two functions currently write HTTP headers independently:

- `gateway_setup_trace()` in `gateway/source/gateway_trace.hpp`
- `apply_auth_to_brpc()` in `gateway/source/gateway_auth.hpp`

Both will be refactored to fill a shared `RpcMetadata` object instead of calling
`SetHeader()`. A new helper serializes and writes the attachment.

```
Gateway request flow (in gateway_server.h):

  1. RpcMetadata meta;
  2. gateway_setup_trace(req, meta)      // fills meta.trace_id
  3. jwt_authenticate(req, res)          // returns user_id/device_id/jti
  4. apply_auth_to_brpc(meta, jwt)       // fills meta.user_id, device_id, jwt_jti
  5. apply_metadata_to_brpc(meta, cntl)  // serializes to cntl->request_attachment()
```

Function signature changes:

- `gateway_setup_trace(HttpServerRequest&, RpcMetadata&)` — no longer depends on Controller
- `apply_auth_to_brpc(RpcMetadata&, JwtPayload&)` — no longer depends on Controller
- New: `apply_metadata_to_brpc(RpcMetadata&, brpc::Controller&)` — serialize + write

Responsibilities stay separated. `gateway_setup_trace` owns tracing concerns;
`apply_auth_to_brpc` owns auth concerns. Both write to the same proto object.
No redundant writes — user_id/device_id may be touched by both functions but only
serialized once.

## 3. Backend Extraction

`common/auth/auth_context.hpp` — `extract_auth()` changes from individual
`http_request().GetHeader()` calls to a single `ParseFromString`:

```cpp
RpcMetadata meta;
if (!meta.ParseFromString(cntl->request_attachment().to_string())) {
    LOG(WARNING) << "Failed to parse RpcMetadata from attachment, first 64 bytes: "
                 << hex_dump(cntl->request_attachment().to_string(), 64);
    throw AuthException("user_id missing");
}

auto user_id   = meta.user_id();
auto device_id = meta.device_id();
auto trace_id  = meta.trace_id();
auto jwt_jti   = meta.jwt_jti();
```

`AuthContext` member variables and the missing-field check (user_id/device_id empty → throw)
remain unchanged.

`detail::read_header()` helper can be removed.

## 4. Inter-Service Forwarding

`common/auth/forward_auth.hpp` — `forward_auth_metadata()` changes from field-level
header copying to opaque attachment copy:

```cpp
void forward_auth_metadata(brpc::Controller& in, brpc::Controller& out) {
    out.request_attachment() = in.request_attachment();
}
```

For services that originate outbound RPCs without an inbound context (e.g., PushService
after receiving a WebSocket message), callers construct `RpcMetadata` directly:

```cpp
RpcMetadata meta;
meta.set_user_id(user_id);
meta.set_device_id(device_id);
// ...
meta.SerializeToString(&data);
cntl->request_attachment().append(data);
```

## 5. Error Handling

| Scenario | Behavior |
|----------|----------|
| attachment is empty | Throw `AuthException("user_id missing")` |
| `ParseFromString` fails | Log warning with hex dump, throw `AuthException` |
| user_id or device_id is empty string | Throw `AuthException` (unchanged) |
| trace_id is empty | No error — trace is best-effort (unchanged) |

## 6. Files Changed

| File | Change | Description |
|------|--------|-------------|
| `common/auth/metadata.proto` | **New** | RpcMetadata proto definition |
| `common/auth/metadata_keys.hpp` | **Remove** | Header key constants no longer needed |
| `common/auth/auth_context.hpp` | **Modify** | `extract_auth()` reads from attachment |
| `common/auth/forward_auth.hpp` | **Modify** | `forward_auth_metadata()` copies attachment |
| `gateway/source/gateway_auth.hpp` | **Modify** | `apply_auth_to_brpc()` fills RpcMetadata |
| `gateway/source/gateway_trace.hpp` | **Modify** | `gateway_setup_trace()` fills RpcMetadata |
| `gateway/source/gateway_server.h` | **Modify** | Orchestrate shared RpcMetadata, call new serialize helper |
| `push/source/push_server.h` | **Modify** | Manual SetHeader → construct RpcMetadata + write attachment |
| `transmite/source/transmite_server.h` | **Modify** | `forward_auth_metadata()` calls, interface unchanged |
| `relationship/source/relationship_server.h` | **Modify** | `forward_auth_metadata()` calls, interface unchanged |
| `conversation/source/conversation_server.h` | **Modify** | `forward_auth_metadata()` calls, interface unchanged |
