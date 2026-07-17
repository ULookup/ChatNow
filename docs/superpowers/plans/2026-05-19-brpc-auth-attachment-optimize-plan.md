# Auth Attachment Chain Optimization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clean up RpcMetadata lifecycle: separate Gateway trace/auth concerns, remove dead extract_auth from Push, seed trace_id for WS ACK path, and improve auth failure logging.

**Architecture:** Five targeted edits across five files. No new files, no proto changes, no API changes. Each task is independent and can be committed separately; the recommended order ensures Gateway changes land before Push changes (shared dependency on `gateway_setup_trace` signature).

**Tech Stack:** C++17, brpc, Protobuf, header-only inline functions

---

### Task 1: gateway_trace.hpp — Remove auth responsibilities

**Files:**
- Modify: `gateway/source/gateway_trace.hpp`

- [ ] **Step 1: Remove user_id/device_id params from gateway_setup_trace**

In `gateway/source/gateway_trace.hpp`, replace the function signature and body at lines 37-52.

**Before (lines 37-52):**
```cpp
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
```

**After:**
```cpp
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       ::chatnow::rpc::RpcMetadata& meta)
{
    std::string trace_id = resolve_trace_id(req);
    meta.set_trace_id(trace_id);
    ::chatnow::log::LogContext::set(trace_id, "", "");
    return trace_id;
}
```

- [ ] **Step 2: Commit (build will fail until Task 3 fixes call sites)**

```bash
git add gateway/source/gateway_trace.hpp
git commit -m "refactor(gateway): remove auth fields from gateway_setup_trace"
```

Note: building `--target gateway` will fail after this commit because `gateway_server.h` still calls the old 4-param signature. Task 3 fixes the call sites — build verification happens there.

---

### Task 2: gateway_auth.hpp — Update LogContext after auth

**Files:**
- Modify: `gateway/source/gateway_auth.hpp`

- [ ] **Step 1: Append LogContext::set to apply_auth_to_brpc**

In `gateway/source/gateway_auth.hpp`, at the end of `apply_auth_to_brpc` (after line 121, before the closing `}`), add a `LogContext::set` call.

**Before (lines 110-122):**
```cpp
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
```

**After:**
```cpp
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
    ::chatnow::log::LogContext::set(
        ::chatnow::log::LogContext::current().trace_id,
        a.user_id, a.device_id);
}
```

Note: `gateway_auth.hpp` already includes `gateway_trace.hpp` which includes `log/log_context.hpp` — no new include needed.

- [ ] **Step 2: Commit**

```bash
git add gateway/source/gateway_auth.hpp
git commit -m "refactor(gateway): update LogContext in apply_auth_to_brpc"
```

Note: building `--target gateway` will fail until Task 3 fixes the call sites. This commit is safe — `apply_auth_to_brpc` signature is unchanged.

---

### Task 3: gateway_server.h — Fix double creation and call sites

**Files:**
- Modify: `gateway/source/gateway_server.h`

- [ ] **Step 1: Update forward() — remove extra gateway_setup_trace params**

In `gateway/source/gateway_server.h`, replace the 4-line block at lines 124-127.

**Before (lines 124-127):**
```cpp
        ::chatnow::rpc::RpcMetadata meta;
        ::chatnow::gateway::gateway_setup_trace(
            httpreq, meta, auth.user_id, auth.device_id);
        ::chatnow::gateway::apply_auth_to_brpc(meta, auth);
```

**After:**
```cpp
        ::chatnow::rpc::RpcMetadata meta;
        ::chatnow::gateway::gateway_setup_trace(httpreq, meta);
        ::chatnow::gateway::apply_auth_to_brpc(meta, auth);
```

- [ ] **Step 2: Update handle_request() — delete meta block, use resolve_trace_id directly**

In `gateway/source/gateway_server.h`, replace the 6-line block at lines 185-190.

**Before (lines 185-190):**
```cpp
        // 写 X-Trace-Id 响应头（trace 已由 forward() 内 gateway_setup_trace 处理，
        // 这里只为 handle_request 自己的日志设置 LogContext）
        ::chatnow::rpc::RpcMetadata meta;
        std::string trace_id = ::chatnow::gateway::gateway_setup_trace(
            req, meta);
        res.set_header("X-Trace-Id", trace_id);
```

**After:**
```cpp
        // 写 X-Trace-Id 响应头
        std::string trace_id = ::chatnow::gateway::resolve_trace_id(req);
        res.set_header("X-Trace-Id", trace_id);
```

- [ ] **Step 3: Verify compilation**

```bash
cmake --build build --target gateway 2>&1 | tail -20
```

Expected: compilation succeeds with no warnings related to these changes.

- [ ] **Step 4: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "refactor(gateway): eliminate double RpcMetadata creation"
```

---

### Task 4: push_server.h — Remove dead auth extraction, seed trace_id

**Files:**
- Modify: `push/source/push_server.h`

- [ ] **Step 1: Add missing include for gen_trace_id**

In `push/source/push_server.h`, after the existing `#include "utils/random_ttl.hpp"` line, add:

```cpp
#include "utils/trace_id.hpp"
```

- [ ] **Step 2: Delete extract_auth from PushToUser**

In `push/source/push_server.h`, delete lines 89-95 inside the `PushToUser` method.

**Delete this block (lines 89-95):**
```cpp
            // auth 提取失败不阻塞（PushToUser 不依赖 auth，所有数据来自 request）
            try {
                auto auth = ::chatnow::auth::extract_auth(cntl);
            } catch (const ::chatnow::ServiceError&) {
                // 内部调用方可能未设置 auth metadata；可接受
            }
```

- [ ] **Step 3: Delete extract_auth from PushBatch**

In `push/source/push_server.h`, delete lines 156-158 inside the `PushBatch` method.

**Delete this block (lines 155-159):**
```cpp
            try {
                auto auth = ::chatnow::auth::extract_auth(cntl);
            } catch (const ::chatnow::ServiceError&) {}
```

Note: the empty line at line 155 (before the try) should also be removed to avoid a double blank line.

- [ ] **Step 4: Add trace_id generation in onClientNotify**

In `push/source/push_server.h`, inside the `onClientNotify` method, after `meta.set_device_id(ack.device_id());` (line 351), add a new line:

```cpp
            meta.set_trace_id(::chatnow::utils::gen_trace_id());
```

The resulting block (lines 348-354) becomes:
```cpp
            // 手动设置 auth metadata：WS handler 无入站 RPC context，需自行构造 RpcMetadata
            ::chatnow::rpc::RpcMetadata meta;
            meta.set_user_id(ack.user_id());
            meta.set_device_id(ack.device_id());
            meta.set_trace_id(::chatnow::utils::gen_trace_id());
            std::string data;
            meta.SerializeToString(&data);
            closure->cntl.request_attachment().append(data);
```

- [ ] **Step 5: Verify compilation**

```bash
cmake --build build --target push 2>&1 | tail -20
```

Expected: compilation succeeds.

- [ ] **Step 6: Commit**

```bash
git add push/source/push_server.h
git commit -m "refactor(push): remove dead extract_auth, seed trace_id in WS ACK path"
```

---

### Task 5: auth_context.hpp — Improve failure log message

**Files:**
- Modify: `common/auth/auth_context.hpp`

- [ ] **Step 1: Include attachment size in error log**

In `common/auth/auth_context.hpp`, replace line 39.

**Before (line 39):**
```cpp
        LOG_WARN("Failed to parse RpcMetadata from attachment");
```

**After:**
```cpp
        LOG_WARN("Failed to parse RpcMetadata from attachment, size={}",
                 cntl ? cntl->request_attachment().size() : 0);
```

- [ ] **Step 2: Verify compilation**

```bash
cmake --build build --target common 2>&1 | tail -20
```

Expected: compilation succeeds.

- [ ] **Step 3: Commit**

```bash
git add common/auth/auth_context.hpp
git commit -m "refactor(auth): include attachment size in extract_auth error log"
```

---

### Task 6: Final verification

- [ ] **Step 1: Full build**

```bash
cmake --build build 2>&1 | tail -30
```

Expected: all targets build successfully, no new warnings.

- [ ] **Step 2: Run existing Go integration tests**

```bash
make -C tests test-func 2>&1 | tail -50
```

Expected: all existing tests pass. Gateway changes are transparent to the HTTP API. Push changes only affect internal paths.

- [ ] **Step 3: Run C++ unit tests (if available)**

```bash
if [ -f build/common/test/common_tests ]; then
    ./build/common/test/common_tests --gtest_filter='*auth*' 2>&1
fi
```

---

### Dependency Order

```
Task 1 (trace.hpp) ──┐
                     ├── Task 3 (server.h) ──┐
Task 2 (auth.hpp)  ──┘                       ├── Task 6 (final check)
                     Task 4 (push) ──────────┤
                     Task 5 (auth_ctx) ──────┘
```

Tasks 1 and 2 can run in parallel. Task 3 depends on both. Tasks 4 and 5 are fully independent.
