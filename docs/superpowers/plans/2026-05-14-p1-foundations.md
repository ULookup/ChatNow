# P1 Foundations 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现横切基础设施 spec 的"地基层"——所有后续 plan 依赖的公共组件（ErrorCode 全集、ServiceError、LogContext、HANDLE_RPC 宏、AuthContext + extract_auth、forward_auth_metadata、metadata key 常量）。**不含 JWT 验签实际逻辑**（属于 P2 Identity 服务）。

**Architecture:** 全部新增到 `common/` 目录下，按职责拆分四个 header-only 模块：`common/error/`（ErrorCode + ServiceError + HANDLE_RPC）、`common/log/`（LogContext + 结构化字段）、`common/auth/`（AuthContext + extract_auth + forward_auth + metadata 常量）。本 plan 不修改任何业务服务代码——仅落地基础组件 + 单元测试。业务服务在 P7 套用这些工具时再改造。

**Tech Stack:** C++17, brpc Controller HTTP headers metadata, protobuf 生成的 `chatnow.common.ErrorCode` enum, spdlog（已有）, gtest（已用于 file/test）。

**Spec 来源:** `docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §5（trace_id+日志+错误处理）+ §2.6 / §2.7（auth_context + forward_auth_metadata）

---

## 0. 依赖前置（在写第一个 task 前确认）

阅读以下文件以掌握项目惯例（任何不一致都按这些惯例来）：
- `common/infra/logger.hpp` — 已有 LOG_INFO/LOG_WARN/LOG_ERROR/LOG_DEBUG 宏，本 plan 不动它们；新加的字段化日志在 `common/log/` 下。
- `file/CMakeLists.txt` — proto 生成 + test target 的标准模板，新加的 test target 沿用此模板。
- `proto/common/error.proto` — 现状仅有部分错误码，本 plan §1 会扩充到全集。
- `common/dao/data_redis.hpp` — 学一下命名风格（snake_case + `inline constexpr` key 前缀），新加的 metadata key 常量沿用此风格。

brpc Controller 的 HTTP headers metadata API 用法（必读 1 分钟）：
- 入站：`cntl->http_request().GetHeader("x-user-id")` 返回 `const std::string*`
- 出站：`cntl->http_request().SetHeader("x-user-id", value)`
- 这是 brpc 官方推荐的元数据传递方式（见 brpc 文档 `http_service.md`）
- 本项目所有服务用 brpc + HTTP/protobuf 协议（看 `gateway_server.h` 即可确认）

---

## File Structure

| 文件 | 职责 | 行数估算 |
|---|---|---|
| `proto/common/error.proto` | ErrorCode enum 全集（修订/扩充） | 50 |
| `common/error/service_error.hpp` | `ServiceError` 异常类 | 40 |
| `common/error/handle_rpc.hpp` | `HANDLE_RPC` 宏 | 60 |
| `common/log/log_context.hpp` | thread_local LogContext (trace_id/user_id/device_id) + 结构化字段拼接帮助 | 80 |
| `common/auth/metadata_keys.hpp` | brpc metadata key 字符串常量（x-trace-id 等） | 25 |
| `common/auth/auth_context.hpp` | `AuthContext` struct + `extract_auth(cntl)` 函数 | 60 |
| `common/auth/forward_auth.hpp` | `forward_auth_metadata(in, out)` 函数 | 40 |
| `common/test/CMakeLists.txt` | 新增 common 单元测试 target | 30 |
| `common/test/test_service_error.cc` | ServiceError 单元测试 | 40 |
| `common/test/test_log_context.cc` | LogContext 单元测试 | 60 |
| `common/test/test_auth_context.cc` | AuthContext + extract_auth 单元测试（用 brpc::Controller 模拟） | 80 |
| `common/test/test_forward_auth.cc` | forward_auth_metadata 单元测试 | 50 |
| `CMakeLists.txt` | 顶层加 `add_subdirectory(common/test)` | 1 |

**单元测试覆盖目标**：每个新增 header 至少一个 happy path + 一个失败路径。

---

## 任务总览

| Task | 标题 | 依赖 |
|---|---|---|
| 1 | 扩充 `proto/common/error.proto` 到 ErrorCode 全集 | 无 |
| 2 | 创建 `common/auth/metadata_keys.hpp`（key 字符串常量） | 无 |
| 3 | 创建 `common/error/service_error.hpp` | T1 |
| 4 | 创建 `common/log/log_context.hpp` | 无 |
| 5 | 创建 `common/auth/auth_context.hpp`（extract_auth 函数） | T2, T3 |
| 6 | 创建 `common/auth/forward_auth.hpp` | T2 |
| 7 | 创建 `common/error/handle_rpc.hpp`（HANDLE_RPC 宏） | T3, T4, T5 |
| 8 | 配置 `common/test/` 测试 target + 顶层 CMakeLists 接入 | T1 |
| 9 | 写 ServiceError 单元测试 | T3, T8 |
| 10 | 写 LogContext 单元测试 | T4, T8 |
| 11 | 写 AuthContext / extract_auth 单元测试 | T5, T8 |
| 12 | 写 forward_auth_metadata 单元测试 | T6, T8 |

每个 Task 自成 commit。所有 Task 完成后 P1 视为落地。

---

### Task 1: 扩充 `proto/common/error.proto` 到 ErrorCode 全集

**Files:**
- Modify: `proto/common/error.proto`

- [ ] **Step 1: 读取现有文件确认起点**

```bash
cat proto/common/error.proto
```

期望看到：当前已有 OK / 1xxx 认证段（部分） / 2xxx / 3xxx / 4xxx / 5xxx (5001-5003) / 6xxx (6001) / 8xxx / 9xxx (9001-9003)。缺失的：1008/1009、5004-5007、7xxx 段、9004。

- [ ] **Step 2: 用以下完整内容覆盖文件**

```protobuf
syntax = "proto3";
package chatnow.common;

enum ErrorCode {
    OK = 0;

    // 1000-1999 认证
    AUTH_INVALID_CREDENTIALS = 1001;
    AUTH_TOKEN_EXPIRED       = 1002;
    AUTH_TOKEN_INVALID       = 1003;
    AUTH_USER_NOT_FOUND      = 1004;
    AUTH_USER_ALREADY_EXISTS = 1005;
    AUTH_VERIFY_CODE_INVALID = 1006;
    AUTH_VERIFY_CODE_EXPIRED = 1007;
    AUTH_REFRESH_TOKEN_REUSED = 1008;
    AUTH_DEVICE_REVOKED       = 1009;

    // 2000-2999 关系
    RELATIONSHIP_ALREADY_FRIENDS = 2001;
    RELATIONSHIP_NOT_FRIENDS     = 2002;
    RELATIONSHIP_BLOCKED         = 2003;
    RELATIONSHIP_REQUEST_PENDING = 2004;

    // 3000-3999 会话
    CONVERSATION_NOT_FOUND     = 3001;
    CONVERSATION_NOT_MEMBER    = 3002;
    CONVERSATION_NO_PERMISSION = 3003;
    CONVERSATION_MEMBER_LIMIT  = 3004;

    // 4000-4999 消息
    MESSAGE_NOT_FOUND        = 4001;
    MESSAGE_RECALL_TIMEOUT   = 4002;
    MESSAGE_ALREADY_RECALLED = 4003;
    MESSAGE_CONTENT_INVALID  = 4004;

    // 5000-5999 媒体
    MEDIA_FILE_TOO_LARGE     = 5001;
    MEDIA_UNSUPPORTED_FORMAT = 5002;
    MEDIA_UPLOAD_FAILED      = 5003;
    MEDIA_QUOTA_EXCEEDED     = 5004;
    MEDIA_HASH_MISMATCH      = 5005;
    MEDIA_UPLOAD_INCOMPLETE  = 5006;
    MEDIA_PART_NOT_FOUND     = 5007;

    // 6000-6999 Presence
    PRESENCE_USER_OFFLINE = 6001;

    // 7000-7999 Device
    DEVICE_NOT_FOUND      = 7001;
    DEVICE_REVOKE_SELF    = 7002;
    DEVICE_LIMIT_EXCEEDED = 7003;

    // 8000-8999 限流
    RATE_LIMIT_EXCEEDED = 8001;

    // 9000-9999 系统
    SYSTEM_INTERNAL_ERROR    = 9001;
    SYSTEM_UNAVAILABLE       = 9002;
    SYSTEM_TIMEOUT           = 9003;
    SYSTEM_INVALID_ARGUMENT  = 9004;
}
```

- [ ] **Step 3: 验证 proto 编译通过**

```bash
cd /Users/yanghaoyang/repo/ChatNow
protoc --cpp_out=/tmp -I proto --experimental_allow_proto3_optional proto/common/error.proto
```

Expected: 无报错。`/tmp/common/error.pb.h` 应被生成，包含所有 30 个 enum 值。

- [ ] **Step 4: 验证整体项目仍可编译（增量构建）**

```bash
cd /Users/yanghaoyang/repo/ChatNow
mkdir -p build && cd build
cmake .. 2>&1 | tail -20
```

Expected: cmake 配置无报错。如果配置成功，跑 `make -j4 2>&1 | tail -40` 看是否编译通过。如果项目需要外部依赖（odb/brpc 等）才能编译，本步骤只要 cmake 配置阶段通过即可，编译错误若来自下游服务用旧错误码则属于预期（P7 才修），列出来跳过。

- [ ] **Step 5: Commit**

```bash
git add proto/common/error.proto
git commit -m "feat(proto): expand ErrorCode to full set (auth/device/media/system)"
```

---

### Task 2: 创建 `common/auth/metadata_keys.hpp`

**Files:**
- Create: `common/auth/metadata_keys.hpp`

- [ ] **Step 1: 创建目录**

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/common/auth
```

- [ ] **Step 2: 写入文件**

```cpp
#pragma once

/**
 * brpc Controller HTTP headers metadata key 常量
 * ---
 * 这些 key 由 Gateway 在鉴权后写入，所有后端服务在 RPC handler 入口
 * 通过 extract_auth() 读取。服务间内部调用必须用 forward_auth_metadata()
 * 透传同名 key。
 *
 * 命名风格：小写 + 横杠（HTTP header 习惯）。brpc 内部对 header 名做大小写
 * 不敏感比较，但写入 / 读取统一用小写避免歧义。
 */

#include <string>

namespace chatnow::auth {

inline constexpr const char* kMetaTraceId  = "x-trace-id";
inline constexpr const char* kMetaUserId   = "x-user-id";
inline constexpr const char* kMetaDeviceId = "x-device-id";
inline constexpr const char* kMetaJwtJti   = "x-jwt-jti";
inline constexpr const char* kMetaClientVer = "x-client-ver";  // 可选

// 内部调用占位值（spec §2.7）
inline constexpr const char* kSystemUserId   = "__system__";
inline constexpr const char* kInternalDeviceId = "__internal__";

}  // namespace chatnow::auth
```

- [ ] **Step 3: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "auth/metadata_keys.hpp"
int main() {
  auto a = chatnow::auth::kMetaTraceId;
  return 0;
}' > /tmp/test_metadata_keys.cc
g++ -std=c++17 -I common /tmp/test_metadata_keys.cc -o /tmp/test_metadata_keys && echo OK
```

Expected: 输出 `OK`。

- [ ] **Step 4: Commit**

```bash
git add common/auth/metadata_keys.hpp
git commit -m "feat(common/auth): add brpc metadata key constants"
```

---

### Task 3: 创建 `common/error/service_error.hpp`

**Files:**
- Create: `common/error/service_error.hpp`

- [ ] **Step 1: 创建目录**

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/common/error
```

- [ ] **Step 2: 写入文件**

```cpp
#pragma once

/**
 * ServiceError —— 业务异常基类
 * ---
 * 业务代码不直接修改 ResponseHeader，而是 throw ServiceError(code, msg)。
 * 由 HANDLE_RPC 宏统一捕获并填充响应。
 *
 * 设计：
 *   - 拷贝 message（非持有 const char*），避免 throw 后字符串失效
 *   - code() 返回原始 int32_t；调用方按需 cast 到 ErrorCode enum
 *     （不依赖 error.pb.h，避免基础设施 header 被 protobuf 污染）
 */

#include <exception>
#include <string>
#include <utility>

namespace chatnow {

class ServiceError : public std::exception {
public:
    ServiceError(int32_t code, std::string message)
        : _code(code), _message(std::move(message)) {}

    int32_t code() const noexcept { return _code; }
    const std::string& message() const noexcept { return _message; }
    const char* what() const noexcept override { return _message.c_str(); }

private:
    int32_t _code;
    std::string _message;
};

}  // namespace chatnow
```

- [ ] **Step 3: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "error/service_error.hpp"
#include <cassert>
int main() {
  try { throw chatnow::ServiceError(1001, "bad"); }
  catch (const chatnow::ServiceError& e) {
    assert(e.code() == 1001);
    assert(e.message() == "bad");
  }
  return 0;
}' > /tmp/test_service_error.cc
g++ -std=c++17 -I common /tmp/test_service_error.cc -o /tmp/test_service_error && /tmp/test_service_error && echo OK
```

Expected: 输出 `OK`。

- [ ] **Step 4: Commit**

```bash
git add common/error/service_error.hpp
git commit -m "feat(common/error): add ServiceError exception type"
```

---

### Task 4: 创建 `common/log/log_context.hpp`

**Files:**
- Create: `common/log/log_context.hpp`

- [ ] **Step 1: 创建目录**

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/common/log
```

- [ ] **Step 2: 写入文件**

```cpp
#pragma once

/**
 * LogContext —— RPC 调用链结构化字段（MDC）
 * ---
 * 每个 RPC handler 入口由 HANDLE_RPC 宏调用 LogContext::set(...)，
 * 退出时 clear()。线程内的所有日志可通过 LogContext::current() 获取
 * trace_id / user_id / device_id 自动拼接。
 *
 * 设计：
 *   - thread_local 存储；brpc 默认每个 RPC 独占一个线程（pthread 模式）
 *     或一个 bthread（M:N 模式）。bthread 也支持 thread_local 语义
 *     （bthread 切换时不会跨 thread_local），所以本设计在 brpc M:N
 *     模式下也是安全的。
 *   - 不依赖 spdlog，独立成单元；P8 trace_id 全链路 plan 会把这些字段
 *     接入 spdlog formatter。本 plan 仅提供存储与读取。
 *   - format_prefix() 输出 "[trace=xxx user=yyy device=zzz] "，业务侧
 *     若想在 LOG_INFO 里手动前缀化可用（P8 之后将自动前缀化）。
 *   - 字段缺失时不写入，不输出空 [trace=]。
 */

#include <string>

namespace chatnow::log {

struct LogFields {
    std::string trace_id;
    std::string user_id;
    std::string device_id;

    bool empty() const { return trace_id.empty() && user_id.empty() && device_id.empty(); }
};

class LogContext {
public:
    static void set(const std::string& trace_id,
                    const std::string& user_id,
                    const std::string& device_id) {
        auto& f = local();
        f.trace_id  = trace_id;
        f.user_id   = user_id;
        f.device_id = device_id;
    }

    static void clear() {
        auto& f = local();
        f.trace_id.clear();
        f.user_id.clear();
        f.device_id.clear();
    }

    static const LogFields& current() { return local(); }

    /* brief: 拼接 "[trace=xxx user=yyy device=zzz] "；缺失字段省略；全空返回 "" */
    static std::string format_prefix() {
        const auto& f = local();
        if (f.empty()) return "";
        std::string out = "[";
        bool need_space = false;
        if (!f.trace_id.empty())  { out += "trace="  + f.trace_id;  need_space = true; }
        if (!f.user_id.empty())   { if (need_space) out += " "; out += "user="   + f.user_id;   need_space = true; }
        if (!f.device_id.empty()) { if (need_space) out += " "; out += "device=" + f.device_id; }
        out += "] ";
        return out;
    }

private:
    static LogFields& local() {
        thread_local LogFields fields;
        return fields;
    }
};

}  // namespace chatnow::log
```

- [ ] **Step 3: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "log/log_context.hpp"
#include <cassert>
int main() {
  using namespace chatnow::log;
  assert(LogContext::current().empty());
  LogContext::set("t1","u1","d1");
  assert(LogContext::current().trace_id == "t1");
  assert(LogContext::format_prefix() == "[trace=t1 user=u1 device=d1] ");
  LogContext::clear();
  assert(LogContext::current().empty());
  return 0;
}' > /tmp/test_log_context.cc
g++ -std=c++17 -I common /tmp/test_log_context.cc -o /tmp/test_log_context && /tmp/test_log_context && echo OK
```

Expected: 输出 `OK`。

- [ ] **Step 4: Commit**

```bash
git add common/log/log_context.hpp
git commit -m "feat(common/log): add LogContext for RPC structured fields"
```

---

### Task 5: 创建 `common/auth/auth_context.hpp`（extract_auth 函数）

**Files:**
- Create: `common/auth/auth_context.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * AuthContext + extract_auth(cntl)
 * ---
 * RPC handler 入口统一调用 extract_auth(cntl) 解析 brpc HTTP headers metadata
 * 中的 x-user-id / x-device-id / x-trace-id（由 Gateway 写入）。
 *
 * 强校验：x-user-id 与 x-device-id 缺失 → throw ServiceError(SYSTEM_INTERNAL_ERROR)。
 *   理由：Gateway 必须写入；缺失说明调用方未透传或 Gateway 出 bug，
 *   不属于业务错误，对客户端而言是 9001 内部错误。
 * 例外：x-trace-id 缺失时使用空字符串（不抛错），理由：内部 worker
 *   可能不带 trace_id；扩散到日志时简单缺一行字段，不影响业务。
 */

#include "auth/metadata_keys.hpp"
#include "error/service_error.hpp"
#include <brpc/controller.h>
#include <string>

namespace chatnow::auth {

struct AuthContext {
    std::string user_id;
    std::string device_id;
    std::string trace_id;
    std::string jwt_jti;       // 可空
};

inline std::string read_header(brpc::Controller* cntl, const char* key) {
    if (!cntl) return "";
    const std::string* v = cntl->http_request().GetHeader(key);
    return v ? *v : "";
}

inline AuthContext extract_auth(brpc::Controller* cntl) {
    AuthContext ctx;
    ctx.user_id   = read_header(cntl, kMetaUserId);
    ctx.device_id = read_header(cntl, kMetaDeviceId);
    ctx.trace_id  = read_header(cntl, kMetaTraceId);
    ctx.jwt_jti   = read_header(cntl, kMetaJwtJti);

    if (ctx.user_id.empty() || ctx.device_id.empty()) {
        throw ServiceError(/*SYSTEM_INTERNAL_ERROR=*/9001,
                           "missing auth metadata: x-user-id / x-device-id required");
    }
    return ctx;
}

}  // namespace chatnow::auth
```

- [ ] **Step 2: 验证编译（依赖 brpc，可能需要 build dir 内编译）**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "auth/auth_context.hpp"
int main() { return 0; }' > /tmp/test_auth_compile.cc
# brpc 头文件路径取决于安装方式，假设标准位置
g++ -std=c++17 -I common -I /usr/local/include -I /usr/include /tmp/test_auth_compile.cc -c -o /tmp/test_auth_compile.o 2>&1 | head -10
```

Expected: 编译成功（仅检查 include 路径与语法），不需要 link。如果 brpc 头文件路径不在标准位置，参考 `gateway/CMakeLists.txt` 的 include_directories 配置补 -I 路径。

- [ ] **Step 3: Commit**

```bash
git add common/auth/auth_context.hpp
git commit -m "feat(common/auth): add AuthContext + extract_auth from brpc metadata"
```

---

### Task 6: 创建 `common/auth/forward_auth.hpp`

**Files:**
- Create: `common/auth/forward_auth.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * forward_auth_metadata(in, out)
 * ---
 * 服务间内部 RPC 调用前调用此函数：把入站 Controller 的 4 个鉴权 metadata
 * 字段（x-trace-id / x-user-id / x-device-id / x-jwt-jti）原样复制到出站
 * Controller。
 *
 * 用于场景：A 服务的 RPC handler 中需要调 B 服务的 RPC，B 服务的 handler
 * 需要知道"原始客户端身份"。透传后 B 服务的 extract_auth(out_cntl)
 * 就能拿到与 A handler 相同的 user_id / device_id。
 */

#include "auth/metadata_keys.hpp"
#include <brpc/controller.h>

namespace chatnow::auth {

inline void copy_header(brpc::Controller* in, brpc::Controller* out, const char* key) {
    if (!in || !out) return;
    const std::string* v = in->http_request().GetHeader(key);
    if (v && !v->empty()) {
        out->http_request().SetHeader(key, *v);
    }
}

inline void forward_auth_metadata(brpc::Controller* in, brpc::Controller* out) {
    copy_header(in, out, kMetaTraceId);
    copy_header(in, out, kMetaUserId);
    copy_header(in, out, kMetaDeviceId);
    copy_header(in, out, kMetaJwtJti);
}

}  // namespace chatnow::auth
```

- [ ] **Step 2: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "auth/forward_auth.hpp"
int main() { return 0; }' > /tmp/test_forward_compile.cc
g++ -std=c++17 -I common -I /usr/local/include -I /usr/include /tmp/test_forward_compile.cc -c -o /tmp/test_forward_compile.o 2>&1 | head -10
```

Expected: 编译成功。

- [ ] **Step 3: Commit**

```bash
git add common/auth/forward_auth.hpp
git commit -m "feat(common/auth): add forward_auth_metadata for inter-service calls"
```

---

### Task 7: 创建 `common/error/handle_rpc.hpp`（HANDLE_RPC 宏）

**Files:**
- Create: `common/error/handle_rpc.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * HANDLE_RPC 宏 —— RPC handler 统一脚手架
 * ---
 * 用法：
 *   void MyServiceImpl::DoX(google::protobuf::RpcController* base_cntl,
 *                           const DoXReq* req, DoXRsp* rsp,
 *                           google::protobuf::Closure* done)
 *   {
 *       brpc::ClosureGuard done_guard(done);
 *       auto* cntl = static_cast<brpc::Controller*>(base_cntl);
 *       HANDLE_RPC(cntl, req, rsp, {
 *           // 业务代码：可使用 auth.user_id / auth.device_id / auth.trace_id
 *           // 失败 throw ServiceError(code, msg)；不要手填 rsp->header()
 *           if (req->name().empty()) {
 *               throw chatnow::ServiceError(9004, "name required");
 *           }
 *           rsp->set_data("hello");
 *       });
 *   }
 *
 * 行为：
 *   1. 入口：extract_auth(cntl) → 缺字段抛 ServiceError(9001)
 *   2. LogContext::set 写入 thread_local
 *   3. body 执行：
 *      - 成功 → header.success=true / error_code=OK / request_id 回填
 *      - throw ServiceError → header.success=false / error_code=e.code() /
 *        error_message=e.message() / WARN 日志
 *      - throw 其他 std::exception → header.error_code=9001 /
 *        error_message="internal error"（不泄漏 what()）/ ERROR 日志
 *   4. LogContext::clear（finally 语义，无论成功/失败/异常都执行）
 *
 * 注意：
 *   - 宏内访问 req->request_id()，要求 req 类型必须有 request_id 字段
 *     （所有 ChatNow 业务 Req 都有此字段）
 *   - 宏内访问 rsp->mutable_header()，要求 rsp 类型必须有 header 字段
 *     （所有 ChatNow 业务 Rsp 都有此字段）
 *   - 不接管 done_guard：调用方仍需 brpc::ClosureGuard done_guard(done)
 */

#include "auth/auth_context.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"
#include "infra/logger.hpp"

#include <exception>

namespace chatnow::detail {

/* finally 守卫：作用域结束时调用 LogContext::clear，异常时也保证执行 */
struct LogContextGuard {
    ~LogContextGuard() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::detail

#define HANDLE_RPC(cntl_ptr, req_ptr, rsp_ptr, body)                        \
    do {                                                                    \
        ::chatnow::detail::LogContextGuard _ctx_guard;                      \
        try {                                                               \
            ::chatnow::auth::AuthContext auth =                             \
                ::chatnow::auth::extract_auth(cntl_ptr);                    \
            ::chatnow::log::LogContext::set(                                \
                auth.trace_id, auth.user_id, auth.device_id);               \
            try {                                                           \
                body                                                        \
                (rsp_ptr)->mutable_header()->set_success(true);             \
                (rsp_ptr)->mutable_header()->set_error_code(0); /*OK*/      \
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
            } catch (const ::chatnow::ServiceError& e) {                    \
                (rsp_ptr)->mutable_header()->set_success(false);            \
                (rsp_ptr)->mutable_header()->set_error_code(e.code());      \
                (rsp_ptr)->mutable_header()->set_error_message(e.message());\
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
                LOG_WARN("rpc_failed code={} msg={}", e.code(),             \
                         e.message().c_str());                              \
            } catch (const std::exception& e) {                             \
                (rsp_ptr)->mutable_header()->set_success(false);            \
                (rsp_ptr)->mutable_header()->set_error_code(9001);          \
                (rsp_ptr)->mutable_header()->set_error_message(             \
                    "internal error");                                      \
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
                LOG_ERROR("rpc_exception what={}", e.what());               \
            }                                                               \
        } catch (const ::chatnow::ServiceError& e) {                        \
            /* extract_auth 抛出（metadata 缺失） */                        \
            (rsp_ptr)->mutable_header()->set_success(false);                \
            (rsp_ptr)->mutable_header()->set_error_code(e.code());          \
            (rsp_ptr)->mutable_header()->set_error_message(e.message());    \
            (rsp_ptr)->mutable_header()->set_request_id(                    \
                (req_ptr)->request_id());                                   \
            LOG_ERROR("rpc_auth_missing code={} msg={}", e.code(),          \
                      e.message().c_str());                                 \
        }                                                                   \
    } while (0)
```

- [ ] **Step 2: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "error/handle_rpc.hpp"
int main() { return 0; }' > /tmp/test_handle_rpc_compile.cc
g++ -std=c++17 -I common -I /usr/local/include -I /usr/include /tmp/test_handle_rpc_compile.cc -c -o /tmp/test_handle_rpc_compile.o 2>&1 | head -10
```

Expected: 编译成功（仅检查语法 + include 链）。

- [ ] **Step 3: Commit**

```bash
git add common/error/handle_rpc.hpp
git commit -m "feat(common/error): add HANDLE_RPC macro for unified handler scaffolding"
```

---

### Task 8: 配置 `common/test/` 测试 target + 顶层 CMakeLists 接入

**Files:**
- Create: `common/test/CMakeLists.txt`
- Modify: `CMakeLists.txt`（顶层）

- [ ] **Step 1: 创建测试目录**

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/common/test
```

- [ ] **Step 2: 创建 `common/test/CMakeLists.txt`**

```cmake
# common 公共组件单元测试
cmake_minimum_required(VERSION 3.1.3)
project(common_tests)

# proto 编译（仅 error.proto，单元测试需要 ErrorCode enum）
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../../proto)
set(proto_files common/error.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path} --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include)

# 一个可执行文件聚合所有 test_*.cc 源文件
set(common_test_target common_tests)
file(GLOB test_srcs ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc)

add_executable(${common_test_target} ${test_srcs} ${proto_srcs})

target_link_libraries(${common_test_target}
    -lgflags -lgtest -lgtest_main
    -lspdlog -lfmt
    -lbrpc -lssl -lcrypto -lprotobuf -lleveldb
    -lcpprest -lcurl
    /usr/local/lib/libjsoncpp.so.19
)

INSTALL(TARGETS ${common_test_target} RUNTIME DESTINATION bin)
```

- [ ] **Step 3: 顶层 `CMakeLists.txt` 加入子目录**

读取顶层 CMakeLists：

```bash
cat /Users/yanghaoyang/repo/ChatNow/CMakeLists.txt
```

在 `add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/push)` 这一行之后追加：

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/common/test)
```

- [ ] **Step 4: 验证 cmake 配置（暂未有源文件，故 GLOB 为空，要么先放占位）**

由于 `file(GLOB test_srcs ...)` 在没有 `test_*.cc` 文件时为空，会导致 `add_executable` 没有源文件而失败。**先放一个占位空 main**：

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/common/test
cat > /Users/yanghaoyang/repo/ChatNow/common/test/test_placeholder.cc <<'EOF'
#include <gtest/gtest.h>
TEST(Placeholder, Sanity) { EXPECT_EQ(1, 1); }
EOF
```

```bash
cd /Users/yanghaoyang/repo/ChatNow && mkdir -p build && cd build && cmake .. 2>&1 | tail -20
```

Expected: 配置成功，看到 `common_tests` target。

- [ ] **Step 5: 编译测试 target**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 2>&1 | tail -40
```

Expected: 编译成功，生成 `common/test/common_tests` 可执行文件。

- [ ] **Step 6: 跑占位测试**

```bash
./common/test/common_tests
```

Expected: 1 test from Placeholder PASSED.

- [ ] **Step 7: Commit**

```bash
cd /Users/yanghaoyang/repo/ChatNow
git add common/test/CMakeLists.txt common/test/test_placeholder.cc CMakeLists.txt
git commit -m "build(common/test): add unit test target for common components"
```

---

### Task 9: 写 ServiceError 单元测试

**Files:**
- Create: `common/test/test_service_error.cc`

- [ ] **Step 1: 写测试（先 fail）**

```cpp
// common/test/test_service_error.cc
#include "error/service_error.hpp"
#include <gtest/gtest.h>

TEST(ServiceError, ConstructAndAccess) {
    chatnow::ServiceError e(1001, "bad credentials");
    EXPECT_EQ(e.code(), 1001);
    EXPECT_EQ(e.message(), "bad credentials");
    EXPECT_STREQ(e.what(), "bad credentials");
}

TEST(ServiceError, ThrowAndCatch) {
    try {
        throw chatnow::ServiceError(9001, "internal");
        FAIL() << "expected throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), 9001);
        EXPECT_EQ(e.message(), "internal");
    } catch (...) {
        FAIL() << "wrong exception type";
    }
}

TEST(ServiceError, MoveSemantics) {
    std::string msg(1024, 'x');
    chatnow::ServiceError e(4001, std::move(msg));
    EXPECT_EQ(e.message().size(), 1024u);
}
```

- [ ] **Step 2: 删除占位文件**

```bash
rm /Users/yanghaoyang/repo/ChatNow/common/test/test_placeholder.cc
```

- [ ] **Step 3: 重新 cmake + 编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && cmake .. && make common_tests -j4 2>&1 | tail -30
```

Expected: 编译成功。

- [ ] **Step 4: 运行**

```bash
./common/test/common_tests --gtest_filter='ServiceError*'
```

Expected: 3 tests passed.

- [ ] **Step 5: Commit**

```bash
cd /Users/yanghaoyang/repo/ChatNow
git add common/test/test_service_error.cc
git rm common/test/test_placeholder.cc
git commit -m "test(common/error): add unit tests for ServiceError"
```

---

### Task 10: 写 LogContext 单元测试

**Files:**
- Create: `common/test/test_log_context.cc`

- [ ] **Step 1: 写测试**

```cpp
// common/test/test_log_context.cc
#include "log/log_context.hpp"
#include <gtest/gtest.h>
#include <thread>

using chatnow::log::LogContext;

class LogContextTest : public ::testing::Test {
protected:
    void TearDown() override { LogContext::clear(); }
};

TEST_F(LogContextTest, EmptyByDefault) {
    EXPECT_TRUE(LogContext::current().empty());
    EXPECT_EQ(LogContext::format_prefix(), "");
}

TEST_F(LogContextTest, SetAndRead) {
    LogContext::set("trace-1", "user-2", "device-3");
    EXPECT_EQ(LogContext::current().trace_id,  "trace-1");
    EXPECT_EQ(LogContext::current().user_id,   "user-2");
    EXPECT_EQ(LogContext::current().device_id, "device-3");
}

TEST_F(LogContextTest, FormatPrefixAllFields) {
    LogContext::set("t", "u", "d");
    EXPECT_EQ(LogContext::format_prefix(), "[trace=t user=u device=d] ");
}

TEST_F(LogContextTest, FormatPrefixPartial) {
    LogContext::set("t", "", "d");
    EXPECT_EQ(LogContext::format_prefix(), "[trace=t device=d] ");
}

TEST_F(LogContextTest, ClearResets) {
    LogContext::set("t","u","d");
    LogContext::clear();
    EXPECT_TRUE(LogContext::current().empty());
}

TEST_F(LogContextTest, ThreadLocalIsolation) {
    LogContext::set("main-trace","main-user","main-dev");

    std::string other_trace_at_set;
    std::thread t([&]() {
        // 子线程刚启动时应当为空（thread_local 各自独立）
        other_trace_at_set = LogContext::current().trace_id;
        LogContext::set("child-trace","child-user","child-dev");
    });
    t.join();

    EXPECT_EQ(other_trace_at_set, "");                       // 子线程默认空
    EXPECT_EQ(LogContext::current().trace_id, "main-trace"); // 主线程不被污染
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 && ./common/test/common_tests --gtest_filter='LogContext*'
```

Expected: 6 tests passed.

- [ ] **Step 3: Commit**

```bash
cd /Users/yanghaoyang/repo/ChatNow
git add common/test/test_log_context.cc
git commit -m "test(common/log): add unit tests for LogContext (incl. thread_local isolation)"
```

---

### Task 11: 写 AuthContext / extract_auth 单元测试

**Files:**
- Create: `common/test/test_auth_context.cc`

- [ ] **Step 1: 写测试**

`brpc::Controller` 可在测试中直接构造并设置 HTTP headers，无需启动真实 server。

```cpp
// common/test/test_auth_context.cc
#include "auth/auth_context.hpp"
#include "auth/metadata_keys.hpp"
#include "error/service_error.hpp"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
brpc::Controller make_cntl(std::initializer_list<std::pair<std::string, std::string>> headers) {
    brpc::Controller cntl;
    for (const auto& kv : headers) {
        cntl.http_request().SetHeader(kv.first, kv.second);
    }
    return cntl;
}
}

TEST(ExtractAuth, AllFieldsPresent) {
    auto cntl = make_cntl({
        {kMetaUserId,   "u_1"},
        {kMetaDeviceId, "d_1"},
        {kMetaTraceId,  "t_1"},
        {kMetaJwtJti,   "jti_1"},
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,   "u_1");
    EXPECT_EQ(ctx.device_id, "d_1");
    EXPECT_EQ(ctx.trace_id,  "t_1");
    EXPECT_EQ(ctx.jwt_jti,   "jti_1");
}

TEST(ExtractAuth, TraceIdOptional) {
    auto cntl = make_cntl({
        {kMetaUserId,   "u_1"},
        {kMetaDeviceId, "d_1"},
        // 无 x-trace-id
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,  "u_1");
    EXPECT_EQ(ctx.trace_id, "");
}

TEST(ExtractAuth, MissingUserIdThrows) {
    auto cntl = make_cntl({
        {kMetaDeviceId, "d_1"},
        {kMetaTraceId,  "t_1"},
    });
    try {
        extract_auth(&cntl);
        FAIL() << "expected throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), 9001);
    }
}

TEST(ExtractAuth, MissingDeviceIdThrows) {
    auto cntl = make_cntl({
        {kMetaUserId,  "u_1"},
        {kMetaTraceId, "t_1"},
    });
    EXPECT_THROW(extract_auth(&cntl), chatnow::ServiceError);
}

TEST(ExtractAuth, NullControllerThrows) {
    EXPECT_THROW(extract_auth(nullptr), chatnow::ServiceError);
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 && ./common/test/common_tests --gtest_filter='ExtractAuth*'
```

Expected: 5 tests passed.

- [ ] **Step 3: Commit**

```bash
cd /Users/yanghaoyang/repo/ChatNow
git add common/test/test_auth_context.cc
git commit -m "test(common/auth): add unit tests for extract_auth (happy/missing fields)"
```

---

### Task 12: 写 forward_auth_metadata 单元测试

**Files:**
- Create: `common/test/test_forward_auth.cc`

- [ ] **Step 1: 写测试**

```cpp
// common/test/test_forward_auth.cc
#include "auth/forward_auth.hpp"
#include "auth/metadata_keys.hpp"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
const std::string* hdr(const brpc::Controller& c, const char* k) {
    return c.http_request().GetHeader(k);
}
std::string read(const brpc::Controller& c, const char* k) {
    auto* v = hdr(c, k);
    return v ? *v : "";
}
}

TEST(ForwardAuthMetadata, CopiesAllFourFields) {
    brpc::Controller in, out;
    in.http_request().SetHeader(kMetaTraceId,  "t");
    in.http_request().SetHeader(kMetaUserId,   "u");
    in.http_request().SetHeader(kMetaDeviceId, "d");
    in.http_request().SetHeader(kMetaJwtJti,   "j");

    forward_auth_metadata(&in, &out);

    EXPECT_EQ(read(out, kMetaTraceId),  "t");
    EXPECT_EQ(read(out, kMetaUserId),   "u");
    EXPECT_EQ(read(out, kMetaDeviceId), "d");
    EXPECT_EQ(read(out, kMetaJwtJti),   "j");
}

TEST(ForwardAuthMetadata, SkipsMissingFields) {
    brpc::Controller in, out;
    in.http_request().SetHeader(kMetaTraceId, "t");
    // 不设置 user_id / device_id / jti

    forward_auth_metadata(&in, &out);

    EXPECT_EQ(read(out, kMetaTraceId),  "t");
    EXPECT_EQ(hdr(out, kMetaUserId),    nullptr);
    EXPECT_EQ(hdr(out, kMetaDeviceId),  nullptr);
    EXPECT_EQ(hdr(out, kMetaJwtJti),    nullptr);
}

TEST(ForwardAuthMetadata, NullSafetyInOrOut) {
    brpc::Controller cntl;
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, &cntl));
    EXPECT_NO_THROW(forward_auth_metadata(&cntl, nullptr));
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, nullptr));
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 && ./common/test/common_tests --gtest_filter='ForwardAuthMetadata*'
```

Expected: 3 tests passed.

- [ ] **Step 3: 跑全部 common_tests 验证**

```bash
./common/test/common_tests
```

Expected: 17 tests passed total（3 ServiceError + 6 LogContext + 5 ExtractAuth + 3 ForwardAuth）。

- [ ] **Step 4: Commit**

```bash
cd /Users/yanghaoyang/repo/ChatNow
git add common/test/test_forward_auth.cc
git commit -m "test(common/auth): add unit tests for forward_auth_metadata"
```

---

## 完成验收

P1 完成后应满足：

- [x] `proto/common/error.proto` 含 30 个 ErrorCode 值
- [x] `common/error/service_error.hpp`、`common/error/handle_rpc.hpp`、`common/log/log_context.hpp`、`common/auth/metadata_keys.hpp`、`common/auth/auth_context.hpp`、`common/auth/forward_auth.hpp` 全部存在
- [x] `common_tests` 可执行，全部 17 个单元测试通过
- [x] 12 个 commit 落入 main 分支
- [x] 现有服务（gateway/message/transmite/...）**编译不破坏**——本 plan 仅新增文件 + 扩 enum，未改动任何业务代码

P2 (JWT 鉴权) / P4 (Media) / P8 (trace_id 透传) 之后会基于这些组件实现。

---

## 自审

**Spec 覆盖检查**（spec §5 + §2.6/§2.7）：
- [x] §5.4 ErrorCode 全集 → Task 1
- [x] §5.5 ServiceError 类 → Task 3
- [x] §5.5 HANDLE_RPC 宏 → Task 7
- [x] §5.6 内部错误不泄漏（other exception → "internal error"）→ Task 7 宏的 catch(std::exception) 分支
- [x] §5.2 LogContext (MDC) → Task 4
- [x] §2.6 AuthContext + extract_auth → Task 5
- [x] §2.7 forward_auth_metadata → Task 6
- [x] §2.7 metadata 4 个 key 字符串 → Task 2

**Spec 中本 plan 不覆盖（明确推到后续 plan）**：
- §5.1 trace_id 生成（在 Gateway 写）→ P8
- §5.1 trace_id 入 MQ headers + WS frame → P8
- §5.2 spdlog JSON formatter → P8（本 plan 仅提供 LogContext 数据源）
- §5.7 客户端重试策略 → 客户端 SDK plan
- §5.8 监控告警接入 → 运维侧
- §2.5 Gateway JWT 验签 → P2

**Placeholder scan**: 无 TBD/TODO/"实现后续补"等占位。

**Type 一致性**: ServiceError::code 返回 `int32_t`（非 enum），所有调用点（HANDLE_RPC 宏、单测）一致。`extract_auth` 返回 `AuthContext`，`forward_auth_metadata` 接受两个 `brpc::Controller*`，与 spec §2.6/§2.7 一致。
