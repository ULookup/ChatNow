# Migrate Auth Metadata to brpc Attachment — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace HTTP header-based auth metadata passing with brpc native `request_attachment()` using Protobuf-serialized `RpcMetadata`.

**Architecture:** New `RpcMetadata` proto carries trace_id/user_id/device_id/jwt_jti. Gateway fills a shared proto object, serializes into `cntl->request_attachment()`. Backend `extract_auth()` reads via `ParseFromString`. Inter-service forwarding does blind IOBuf copy. All 9 service CMakeLists add the new proto.

**Tech Stack:** Protobuf, brpc baidu_std protocol, C++17

---

### Task 1: Create RpcMetadata proto

**Files:**
- Create: `proto/common/auth/metadata.proto`

- [ ] **Step 1: Create proto directory and file**

```bash
mkdir -p proto/common/auth
```

- [ ] **Step 2: Write proto definition**

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

- [ ] **Step 3: Commit**

```bash
git add proto/common/auth/metadata.proto
git commit -m "feat: add RpcMetadata proto for brpc attachment-based auth"
```

---

### Task 2: Add metadata.proto to all service CMakeLists.txt

**Files:**
- Modify: `gateway/CMakeLists.txt`
- Modify: `push/CMakeLists.txt`
- Modify: `identity/CMakeLists.txt`
- Modify: `message/CMakeLists.txt`
- Modify: `transmite/CMakeLists.txt`
- Modify: `presence/CMakeLists.txt`
- Modify: `relationship/CMakeLists.txt`
- Modify: `conversation/CMakeLists.txt`
- Modify: `media/CMakeLists.txt`

- [ ] **Step 1: gateway/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In line 10, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto identity/identity_service.proto relationship/relationship_service.proto conversation/conversation_service.proto message/message_types.proto message/message_service.proto media/media_service.proto presence/presence_service.proto push/push_service.proto push/notify.proto transmite/transmite_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto identity/identity_service.proto relationship/relationship_service.proto conversation/conversation_service.proto message/message_types.proto message/message_service.proto media/media_service.proto presence/presence_service.proto push/push_service.proto push/notify.proto transmite/transmite_service.proto)
```

- [ ] **Step 2: push/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In line 7, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto message/message_types.proto message/message_service.proto message/message_internal.proto push/push_service.proto push/notify.proto presence/presence_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto message/message_types.proto message/message_service.proto message/message_internal.proto push/push_service.proto push/notify.proto presence/presence_service.proto)
```

- [ ] **Step 3: identity/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In line 10, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto identity/identity_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto identity/identity_service.proto)
```

- [ ] **Step 4: message/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In line 10, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto identity/identity_service.proto media/media_service.proto message/message_types.proto message/message_internal.proto message/message_service.proto push/notify.proto push/push_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto identity/identity_service.proto media/media_service.proto message/message_types.proto message/message_internal.proto message/message_service.proto push/notify.proto push/push_service.proto)
```

- [ ] **Step 5: transmite/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In line 10, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto message/message_types.proto identity/identity_service.proto conversation/conversation_service.proto message/message_service.proto message/message_internal.proto transmite/transmite_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto message/message_types.proto identity/identity_service.proto conversation/conversation_service.proto message/message_service.proto message/message_internal.proto transmite/transmite_service.proto)
```

- [ ] **Step 6: presence/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In lines 7-15, change the proto_files set to include `common/auth/metadata.proto` as first item:
```cmake
set(proto_files
    common/auth/metadata.proto
    common/types.proto
    common/error.proto
    common/envelope.proto
    message/message_types.proto
    presence/presence_service.proto
    push/push_service.proto
    push/notify.proto
)
```

- [ ] **Step 7: relationship/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In lines 8-12, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto
                message/message_types.proto
                identity/identity_service.proto
                conversation/conversation_service.proto
                relationship/relationship_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto
                common/auth/metadata.proto
                message/message_types.proto
                identity/identity_service.proto
                conversation/conversation_service.proto
                relationship/relationship_service.proto)
```

- [ ] **Step 8: conversation/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In lines 8-12, change:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto
                message/message_types.proto
                message/message_service.proto
                identity/identity_service.proto
                conversation/conversation_service.proto)
```
to:
```cmake
set(proto_files common/types.proto common/error.proto common/envelope.proto
                common/auth/metadata.proto
                message/message_types.proto
                message/message_service.proto
                identity/identity_service.proto
                conversation/conversation_service.proto)
```

- [ ] **Step 9: media/CMakeLists.txt — add `common/auth/metadata.proto` to proto_files**

In lines 11-15, change:
```cmake
set(proto_files
    common/types.proto
    common/error.proto
    common/envelope.proto
    media/media_service.proto)
```
to:
```cmake
set(proto_files
    common/auth/metadata.proto
    common/types.proto
    common/error.proto
    common/envelope.proto
    media/media_service.proto)
```

- [ ] **Step 10: Commit**

```bash
git add gateway/CMakeLists.txt push/CMakeLists.txt identity/CMakeLists.txt message/CMakeLists.txt transmite/CMakeLists.txt presence/CMakeLists.txt relationship/CMakeLists.txt conversation/CMakeLists.txt media/CMakeLists.txt
git commit -m "build: add metadata.proto to all service CMakeLists"
```

---

### Task 3: Rebuild auth_context.hpp to read from attachment

**Files:**
- Modify: `common/auth/auth_context.hpp`

- [ ] **Step 1: Replace file content**

Replace entire content of `common/auth/auth_context.hpp` with:

```cpp
#pragma once

/**
 * AuthContext + extract_auth(cntl)
 * ---
 * RPC handler 入口统一调用 extract_auth(cntl) 解析 brpc request_attachment
 * 中的 RpcMetadata（由 Gateway 写入）。
 *
 * 强校验：x-user-id 与 x-device-id 缺失 → throw ServiceError(SYSTEM_INTERNAL_ERROR)。
 *   理由：Gateway 必须写入；缺失说明调用方未透传或 Gateway 出 bug，
 *   不属于业务错误，对客户端而言是 9001 内部错误。
 * 例外：x-trace-id 缺失时使用空字符串（不抛错），理由：内部 worker
 *   可能不带 trace_id；扩散到日志时简单缺一行字段，不影响业务。
 */

#include "common/auth/metadata.pb.h"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"
#include <brpc/controller.h>
#include <string>

namespace chatnow::auth {

struct AuthContext {
    std::string user_id;
    std::string device_id;
    std::string trace_id;
    std::string jwt_jti;       // 可空
};

inline AuthContext extract_auth(brpc::Controller* cntl) {
    chatnow::rpc::RpcMetadata meta;
    bool ok = false;
    if (cntl) {
        ok = meta.ParseFromString(cntl->request_attachment().to_string());
    }
    if (!ok) {
        LOG_WARN("Failed to parse RpcMetadata from attachment");
        throw ServiceError(::chatnow::error::kSystemInternalError,
                           "missing auth metadata: user_id/device_id required");
    }

    AuthContext ctx;
    ctx.user_id   = meta.user_id();
    ctx.device_id = meta.device_id();
    ctx.trace_id  = meta.trace_id();
    ctx.jwt_jti   = meta.jwt_jti();

    if (ctx.user_id.empty() || ctx.device_id.empty()) {
        throw ServiceError(::chatnow::error::kSystemInternalError,
                           "missing auth metadata: user_id/device_id required");
    }
    return ctx;
}

}  // namespace chatnow::auth
```

- [ ] **Step 2: Commit**

```bash
git add common/auth/auth_context.hpp
git commit -m "refactor(auth): read RpcMetadata from brpc request_attachment"
```

---

### Task 4: Rebuild forward_auth.hpp to copy attachment

**Files:**
- Modify: `common/auth/forward_auth.hpp`

- [ ] **Step 1: Replace file content**

Replace entire content of `common/auth/forward_auth.hpp` with:

```cpp
#pragma once

/**
 * forward_auth_metadata(in, out)
 * ---
 * 服务间内部 RPC 调用前调用此函数：把入站 Controller 的 request_attachment
 * 原样复制到出站 Controller。
 *
 * 用于场景：A 服务的 RPC handler 中需要调 B 服务的 RPC，B 服务的 handler
 * 需要知道"原始客户端身份"。透传后 B 服务的 extract_auth(out_cntl)
 * 就能拿到与 A handler 相同的 user_id / device_id。
 */

#include <brpc/controller.h>

namespace chatnow::auth {

inline void forward_auth_metadata(brpc::Controller* in, brpc::Controller* out) {
    if (!in || !out) return;
    out->request_attachment() = in->request_attachment();
}

}  // namespace chatnow::auth
```

- [ ] **Step 2: Commit**

```bash
git add common/auth/forward_auth.hpp
git commit -m "refactor(auth): forward auth by copying request_attachment"
```

---

### Task 5: Remove metadata_keys.hpp

**Files:**
- Remove: `common/auth/metadata_keys.hpp`

- [ ] **Step 1: Delete file**

```bash
rm common/auth/metadata_keys.hpp
```

- [ ] **Step 2: Commit**

```bash
git add common/auth/metadata_keys.hpp
git commit -m "refactor(auth): remove metadata_keys.hpp, superseded by RpcMetadata proto"
```

---

### Task 6: Rebuild gateway_trace.hpp to fill RpcMetadata

**Files:**
- Modify: `gateway/source/gateway_trace.hpp`

- [ ] **Step 1: Replace file content**

Replace entire content of `gateway/source/gateway_trace.hpp` with:

```cpp
#pragma once

/**
 * gateway_setup_trace
 * ---
 * Gateway 每个 HTTP handler 入口三件套：
 *   1. 从 HTTP 请求读 X-Trace-Id；不合法则现生成 32 字符 hex
 *   2. 写到 RpcMetadata（传引用，与 apply_auth_to_brpc 共享同一对象）
 *   3. LogContext::set(trace_id, user_id, device_id)
 *      让 Gateway 自身的 LOG_xxx 输出也带 trace_id
 */

#include "log/log_context.hpp"
#include "utils/trace_id.hpp"
#include "common/auth/metadata.pb.h"

#include "httplib.h"
#include <string>

namespace chatnow::gateway {

/* brief: 解析 X-Trace-Id；不合法或缺失则现生成 */
inline std::string resolve_trace_id(const httplib::Request& req) {
    auto it = req.headers.find("X-Trace-Id");
    if (it != req.headers.end()) {
        if (::chatnow::utils::is_valid_trace_id(it->second)) {
            return it->second;
        }
    }
    return ::chatnow::utils::gen_trace_id();
}

/* brief: 一行接入：解析 trace_id → 填 RpcMetadata → 写 LogContext
 *   返回 trace_id（调用方按需用，例如填回 HTTP response header 给客户端）
 *   user_id/device_id 可空；非空时也填入 meta。
 */
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       ::chatnow::rpc::RpcMetadata& meta,
                                       const std::string& user_id = "",
                                       const std::string& device_id = "")
{
    std::string trace_id = resolve_trace_id(req);
    meta.set_trace_id(trace_id);
    if (!user_id.empty()) {
        meta.set_user_id(user_id);
    }
    if (!device_id.empty()) {
        meta.set_device_id(device_id);
    }
    ::chatnow::log::LogContext::set(trace_id, user_id, device_id);
    return trace_id;
}

/* brief: handler 退出 RAII 守卫；脱离作用域时 clear LogContext */
struct LogContextScope {
    ~LogContextScope() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::gateway
```

- [ ] **Step 2: Commit**

```bash
git add gateway/source/gateway_trace.hpp
git commit -m "refactor(gateway): fill RpcMetadata in gateway_setup_trace"
```

---

### Task 7: Rebuild gateway_auth.hpp to fill RpcMetadata

**Files:**
- Modify: `gateway/source/gateway_auth.hpp`

- [ ] **Step 1: Replace file content**

Replace entire content of `gateway/source/gateway_auth.hpp` with:

```cpp
#pragma once

/**
 * gateway_auth — JWT 鉴权中间件 + RpcMetadata 写入
 * 横切 spec §2.5
 *
 * 入口契约（每个 handler 顶部一行）：
 *
 *   chatnow::gateway::LogContextScope _trace_scope;
 *   chatnow::rpc::RpcMetadata meta;
 *   AuthInfo a;
 *   if (!chatnow::gateway::jwt_authenticate(request, response, _jwt_codec,
 *                                            _jwt_store, /*whitelisted=*\/false, a)) {
 *       return;  // 401 已写
 *   }
 *   ...
 *   chatnow::gateway::apply_auth_to_brpc(meta, a);
 *   chatnow::gateway::apply_metadata_to_brpc(meta, cntl);
 */

#include "auth/jwt_codec.hpp"
#include "auth/jwt_store.hpp"
#include "common/auth/metadata.pb.h"
#include "common/envelope.pb.h"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "gateway_trace.hpp"
#include "infra/logger.hpp"

#include "httplib.h"
#include <brpc/controller.h>

#include <memory>
#include <string>

namespace chatnow::gateway {

struct AuthInfo {
    bool        authed = false;
    std::string user_id;
    std::string device_id;
    std::string jwt_jti;
};

/* brief: 解析 Authorization Bearer + 验签 + 黑名单检查
 *  whitelisted=true → 直接返回 true 且 authed=false（仅 trace_id 流程）
 *  失败时写 401 + ResponseHeader 风格 body，返回 false
 */
inline bool jwt_authenticate(const httplib::Request& request,
                             httplib::Response& response,
                             const std::shared_ptr<::chatnow::auth::JwtCodec>& codec,
                             const std::shared_ptr<::chatnow::auth::JwtStore>& store,
                             bool whitelisted,
                             AuthInfo& out)
{
    if (whitelisted) {
        out.authed = false;
        return true;
    }

    auto write_401 = [&](int32_t code, const std::string& msg) {
        ::chatnow::common::ResponseHeader rsp;
        rsp.set_success(false);
        rsp.set_error_code(code);
        rsp.set_error_message(msg);
        response.status = 401;
        response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
    };

    auto it = request.headers.find("Authorization");
    if (it == request.headers.end()) {
        LOG_WARN("缺 Authorization header path={}", request.path);
        write_401(::chatnow::error::kAuthTokenInvalid, "missing Authorization");
        return false;
    }
    static const std::string kPrefix = "Bearer ";
    const std::string& auth_header = it->second;
    if (auth_header.size() <= kPrefix.size() ||
        auth_header.compare(0, kPrefix.size(), kPrefix) != 0) {
        write_401(::chatnow::error::kAuthTokenInvalid, "missing Bearer prefix");
        return false;
    }
    std::string token = auth_header.substr(kPrefix.size());

    try {
        auto claims = codec->verify(token, /*require_refresh=*/false);
        if (store->is_revoked(claims.jti)) {
            write_401(::chatnow::error::kAuthTokenInvalid, "token revoked");
            return false;
        }
        out.authed    = true;
        out.user_id   = claims.sub;
        out.device_id = claims.did;
        out.jwt_jti   = claims.jti;
        return true;
    } catch (const ::chatnow::ServiceError& e) {
        LOG_WARN("JWT 验签失败 path={} code={} msg={}",
                 request.path, e.code(), e.message());
        write_401(e.code(), e.message());
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("JWT 验签异常 path={}: {}", request.path, e.what());
        write_401(::chatnow::error::kSystemInternalError, "auth internal error");
        return false;
    }
}

/* brief: 将 JWT claims 写入 RpcMetadata（user_id, device_id, jwt_jti）
 */
inline void apply_auth_to_brpc(::chatnow::rpc::RpcMetadata& meta,
                               const AuthInfo& a)
{
    if (!a.user_id.empty()) {
        meta.set_user_id(a.user_id);
    }
    if (!a.device_id.empty()) {
        meta.set_device_id(a.device_id);
    }
    if (a.authed && !a.jwt_jti.empty()) {
        meta.set_jwt_jti(a.jwt_jti);
    }
}

/* brief: 把 RpcMetadata 序列化写入 brpc Controller 的 request_attachment
 */
inline void apply_metadata_to_brpc(const ::chatnow::rpc::RpcMetadata& meta,
                                   brpc::Controller& cntl)
{
    std::string data;
    meta.SerializeToString(&data);
    cntl.request_attachment().append(data);
}

}  // namespace chatnow::gateway
```

- [ ] **Step 2: Commit**

```bash
git add gateway/source/gateway_auth.hpp
git commit -m "refactor(gateway): fill RpcMetadata in apply_auth_to_brpc"
```

---

### Task 8: Rebuild gateway_server.h to orchestrate RpcMetadata flow

**Files:**
- Modify: `gateway/source/gateway_server.h` (lines 99-133, 157-187)

- [ ] **Step 1: Update forward() method — create RpcMetadata, use new API**

In `forward()`, replace lines 120-124:
```cpp
        Stub stub(ch.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms);
        ::chatnow::gateway::apply_auth_to_brpc(httpreq, cntl, auth);
        (stub.*method)(&cntl, &pb_req, &pb_rsp, nullptr);
```

With:
```cpp
        Stub stub(ch.get());
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout_ms);

        ::chatnow::rpc::RpcMetadata meta;
        ::chatnow::gateway::gateway_setup_trace(
            httpreq, meta, auth.user_id, auth.device_id);
        ::chatnow::gateway::apply_auth_to_brpc(meta, auth);
        ::chatnow::gateway::apply_metadata_to_brpc(meta, cntl);

        (stub.*method)(&cntl, &pb_req, &pb_rsp, nullptr);
```

- [ ] **Step 2: Update handle_request() — stop dummy controller, populate meta**

In `handle_request()`, replace lines 179-186:
```cpp
        // 写 X-Trace-Id 响应头
        brpc::Controller dummy_cntl;
        std::string trace_id = ::chatnow::gateway::gateway_setup_trace(
            req, dummy_cntl, a.user_id, a.device_id);
        res.set_header("X-Trace-Id", trace_id);

        // 转发
        matched->handler(req, res, a, _channels, matched->timeout_ms, dummy_cntl);
```

With:
```cpp
        // 写 X-Trace-Id 响应头（trace 已由 forward() 内 gateway_setup_trace 处理，
        // 这里只为 handle_request 自己的日志设置 LogContext）
        ::chatnow::rpc::RpcMetadata meta;
        std::string trace_id = ::chatnow::gateway::gateway_setup_trace(
            req, meta);
        res.set_header("X-Trace-Id", trace_id);

        // 转发（forward() 内部会创建自己的 RpcMetadata 并写入 attachment）
        brpc::Controller dummy_cntl;
        matched->handler(req, res, a, _channels, matched->timeout_ms, dummy_cntl);
```

- [ ] **Step 3: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "refactor(gateway): orchestrate RpcMetadata through gateway handlers"
```

---

### Task 9: Rebuild push_server.h for attachment writes

**Files:**
- Modify: `push/source/push_server.h` (lines 348-352)

- [ ] **Step 1: Replace manual SetHeader with RpcMetadata write**

In `onClientNotify()`, replace lines 348-352:
```cpp
            // 手动设置 auth headers：WS handler 无入站 RPC context，extract_auth 需这些字段
            closure->cntl.http_request().SetHeader("x-user-id", ack.user_id());
            closure->cntl.http_request().SetHeader("x-device-id", ack.device_id());
            closure->cntl.http_request().SetHeader("x-trace-id", "");
            closure->cntl.http_request().SetHeader("x-jwt-jti", "");
```

With:
```cpp
            // 手动设置 auth metadata：WS handler 无入站 RPC context，需自行构造 RpcMetadata
            ::chatnow::rpc::RpcMetadata meta;
            meta.set_user_id(ack.user_id());
            meta.set_device_id(ack.device_id());
            std::string data;
            meta.SerializeToString(&data);
            closure->cntl.request_attachment().append(data);
```

- [ ] **Step 2: Commit**

```bash
git add push/source/push_server.h
git commit -m "refactor(push): write RpcMetadata to attachment for WS-originated RPCs"
```

---

### Task 10: Build and verify

- [ ] **Step 1: Build all services**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tee build.log
```

Expect: All 9 targets (`gateway_server`, `push_server`, `identity_server`, `message_server`, `transmite_server`, `presence_server`, `relationship_server`, `conversation_server`, `media_server`) compile successfully.

- [ ] **Step 2: Check for any remaining references to old metadata_keys or SetHeader patterns**

```bash
grep -rn "kMetaTraceId\|kMetaUserId\|kMetaDeviceId\|kMetaJwtJti\|kMetaClientVer" --include="*.hpp" --include="*.h" --include="*.cpp" --include="*.cc"
grep -rn "metadata_keys.hpp" --include="*.hpp" --include="*.h" --include="*.cpp" --include="*.cc"
grep -rn 'SetHeader.*x-user-id\|SetHeader.*x-device-id\|SetHeader.*x-trace-id\|SetHeader.*x-jwt-jti' --include="*.hpp" --include="*.h" --include="*.cpp" --include="*.cc"
```

Expect: No output (all old references cleaned up).

- [ ] **Step 3: Commit build verification**

```bash
git add -A
git commit -m "chore: build verification after auth attachment migration"
```
