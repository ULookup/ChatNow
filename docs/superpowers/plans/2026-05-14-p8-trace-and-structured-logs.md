# P8 trace_id 端到端透传 + spdlog JSON 结构化日志 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 trace_id 从 Gateway 入口贯通到所有后端 RPC、MQ headers、WS 推送帧，让运维侧可按一个 32 字符 hex id 串联整条消息链路；同时把 spdlog 现有"自由文本 + 文件名行号"的输出格式切到结构化 JSON，让 ELK/Loki 可直接索引 trace_id / user_id / device_id / level / msg / fields。

**Architecture:**
- **trace_id 生成**：仅 Gateway 生成。客户端可送 `X-Trace-Id` HTTP header 透传；不合法或缺失则 Gateway 调 `gen_trace_id()` 现生成（16 字节随机 → 32 字符小写 hex）。
- **透传链路**：
  ```
  HTTP(X-Trace-Id) → Gateway httplib handler
                   → brpc::Controller HTTP header (x-trace-id)  [P1 已落 metadata key/读写]
                   → 后端 RPC handler 经 HANDLE_RPC 宏写入 LogContext  [P1 已完成]
                   → 服务间内部调用经 forward_auth_metadata 透传   [P1 已完成]
                   → MQ publish_confirm 时把 trace_id 注入 AMQP headers
                   → MQ consume 回调起首读 headers 并 LogContext::set
                   → Push 服务构造 NotifyMessage 时填 trace_id 字段
                   → 客户端 SDK 收到帧后绑定到本地日志
  ```
- **JSON 日志**：在 `common/infra/logger.hpp` 中 `init_logger` 时把 spdlog pattern 设为 `%v`，每个 LOG_xxx 宏在调用 `g_default_logger->log` 之前先把内容拼成完整 JSON 一行。即"宏侧自构 JSON + sink 透明输出"，不引入第三方 JSON 库（项目无 nlohmann/json）。
- **保持向后兼容**：现有 `LOG_INFO("user logged in: {}", uid)` 仍能编译运行，输出从 `[file:line] user logged in: 42` 变为 `{"ts":...,"level":"info","msg":"user logged in: 42",...}`。新增 `LOG_INFOF("event_name", {{"k","v"}, ...})` 风格字段化变体。
- **范围之外**：JWT 鉴权（P2）、ServiceError / HANDLE_RPC 宏（P1 已完成）、proto 业务体清理（P7）。

**Tech Stack:** C++17, spdlog（已用，pattern_formatter API）, AMQP-CPP（headers via `AMQP::Envelope::setHeader`）, brpc HTTP header metadata（P1 已铺路）, gtest。

**Spec 来源:** `docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §5（trace_id + 日志 + 错误处理 生产版）。本 plan 仅覆盖 §5.1 / §5.2 / §5.7（仅文档）/ §5.8（仅文档）。§5.3 / §5.4 / §5.5 / §5.6 已由 P1 落地。

---

## 0. 依赖前置（在写第一个 Task 前必读）

阅读以下文件以掌握项目惯例（任何不一致都按这些惯例来）：

- `common/infra/logger.hpp` — 已有 `init_logger`、`g_default_logger`、`LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL` 宏；本 plan 要在保留宏名/参数列表的前提下重写宏体与 `init_logger` 内的 pattern。第 84-89 行是当前宏，第 72 行是当前 pattern。
- `common/auth/metadata_keys.hpp` (P1) — 已含 `kMetaTraceId = "x-trace-id"`，本 plan 复用。
- `common/auth/auth_context.hpp` (P1) — `extract_auth` 已读取 trace_id 写入 `AuthContext::trace_id`。
- `common/auth/forward_auth.hpp` (P1) — 服务间内部调用透传 4 个 metadata 字段。
- `common/log/log_context.hpp` (P1) — `LogContext::set/clear/current/format_prefix`，本 plan 在 JSON 输出里读 `current()`。
- `common/error/handle_rpc.hpp` (P1) — `HANDLE_RPC` 宏已经在 RPC 入口调 `LogContext::set`，无需在 P8 重复。
- `common/mq/rabbitmq.hpp` — `MQClient::publish_confirm`、`Publisher::publish_confirm`、`MessageCallback`（消费回调签名 `(const char*, size_t, bool)`）；本 plan 要为发送侧增 headers 入参，为消费侧暴露 headers。
- `common/utils/utils.hpp` — 项目工具函数风格（小写 + snake_case + `inline`）。
- `gateway/source/gateway_server.h` — 看 `GetMailVerifyCode` (line 213-238)、`UserRegister`、`UserLogin` 的 handler 标准模板：解 req → choose channel → `brpc::Controller cntl` → stub.Method(&cntl, ...) → 序列化 rsp。本 plan 要在每个 handler 入口插入"读 X-Trace-Id 或生成新值 → 写到 cntl 的 HTTP header → LogContext::set" 三件套，封装成 `gateway_setup_trace(...)`。
- `proto/push/notify.proto` — 当前 `NotifyMessage` 有 13 个 oneof slot（field 1-13），本 plan 需在 `NotifyMessage` 顶层新增 `optional string trace_id = 14`，不进 oneof。

AMQP-CPP headers API（必读 1 分钟）：

- 发布：`AMQP::Envelope env(body, size); env.setHeader("trace_id", value);` 然后 `_channel.publish(exchange, key, env)`。`AMQP::Reliable<>::publish` 接受 `Envelope` 的重载。
- 消费：回调签名 `(const AMQP::Message& m, uint64_t tag, bool redeliv)`，用 `m.headers()` 拿 `AMQP::Table&`；`headers.get("trace_id")` 返回 `AMQP::Field`，可 `.toString()`。
- 详见 AMQP-CPP 头文件 `<amqpcpp/envelope.h>` / `<amqpcpp/message.h>`。

spdlog pattern_formatter（必读 30 秒）：

- 调用 `logger->set_pattern("%v")` 让 spdlog 仅把 message 原样输出（不再加时间/线程/level 前缀）。日志的所有结构由 LOG_xxx 宏自己组装。
- 不使用 `spdlog::custom_flag_formatter`，理由：自定义 flag 仍要走 spdlog formatter pipeline，对 `%v` 多次拷贝；自构 JSON 字符串一次到位更简单可读。

---

## File Structure

| 文件 | 动作 | 行数估算 |
|---|---|---|
| `common/utils/trace_id.hpp` | 新增：`gen_trace_id()` / `is_valid_trace_id()` | 60 |
| `common/infra/log_json.hpp` | 新增：JSON 转义 / `build_log_line(level, msg, fields)` | 90 |
| `common/infra/logger.hpp` | 修改：`init_logger` 切 pattern；`set_service_name`；重写 LOG_xxx 宏；新增 LOG_INFOF/WARNF/ERRORF/DEBUGF | 150（在原 92 行基础上） |
| `common/mq/rabbitmq.hpp` | 修改：`MQClient::publish_confirm` / `Publisher::publish_confirm` 增 `headers` 入参；`consume` 回调签名增加 `const std::map<std::string,std::string>&` headers 参数，并保持旧签名重载（调用方不破坏） | 改 +60 行 |
| `common/mq/trace_headers.hpp` | 新增：`mq_inject_trace_headers(map&)` / `mq_extract_trace_id(headers)` | 50 |
| `gateway/source/gateway_trace.hpp` | 新增：`gateway_setup_trace(httplib_request, brpc::Controller&) → string` | 70 |
| `gateway/source/gateway_server.h` | 修改：每个 HTTP handler 在 `brpc::Controller cntl;` 之后调用 `gateway_setup_trace(...)`；尾部 `LogContext::clear()`。1 个 handler 改造为示例 + 1 个机械 sed 任务把剩余 33 个 handler 同样改造 | 改约 100 行（每 handler +3 行） |
| `proto/push/notify.proto` | 修改：`NotifyMessage` 新增 `optional string trace_id = 14;` | +1 行 |
| `transmite/source/transmite_server.h` | 修改：`publish_confirm` 调用处把当前 `LogContext::current().trace_id` 注入 headers | 改 +5 行 |
| `message/source/message_server.h` | 修改：MQ 消费回调起首调 `mq_extract_trace_id` + `LogContext::set`；尾部 `LogContext::clear`；下游 push/es publish 同样注入 headers | 改 +30 行 |
| `push/source/push_server.h`（或 push 主消费回调所在文件） | 修改：MQ 消费回调起首读 trace_id；构造 `NotifyMessage` 时 `set_trace_id(...)` | 改 +15 行 |
| `common/test/test_trace_id.cc` | 新增 | 50 |
| `common/test/test_log_json.cc` | 新增 | 90 |
| `common/test/test_mq_trace_headers.cc` | 新增 | 60 |
| `common/test/CMakeLists.txt` | 不修改（GLOB 自动 include 新 test_*.cc） | 0 |
| `docs/operations/log-format.md` | 新增（spec §5.2 文档化） | 80 |
| `docs/operations/log-levels.md` | 新增（spec §5.2 文档化） | 60 |
| `docs/operations/monitoring-conventions.md` | 新增（spec §5.8 文档化） | 50 |
| `docs/client-sdk/error-retry.md` | 新增（spec §5.7 文档化） | 80 |

**单元测试覆盖目标**：trace_id 生成器（格式 + 唯一性 + 校验），JSON 转义（控制字符 / 引号 / 反斜杠），MQ trace headers 注入/提取（含缺失场景）。

---

## 任务总览

| Task | 标题 | 依赖 |
|---|---|---|
| 1 | 新增 `common/utils/trace_id.hpp` —— 32 字符 hex 生成器 | 无 |
| 2 | 单元测试：trace_id 生成与校验 | T1 |
| 3 | 新增 `common/infra/log_json.hpp` —— JSON 转义 + build_log_line | T1 |
| 4 | 单元测试：log_json 转义边界 | T3 |
| 5 | 改写 `common/infra/logger.hpp` —— pattern 切 `%v`、`set_service_name`、宏改 JSON、新增 LOG_xxxF 字段化变体 | T3 |
| 6 | 新增 `common/mq/trace_headers.hpp` —— MQ 头部 trace_id 注入/提取 | T1 |
| 7 | 单元测试：mq_trace_headers | T6 |
| 8 | 改造 `common/mq/rabbitmq.hpp` —— publish_confirm 加 headers；consume 回调暴露 headers | 无 |
| 9 | 修改 `proto/push/notify.proto` —— `NotifyMessage.trace_id = 14` | 无 |
| 10 | 新增 `gateway/source/gateway_trace.hpp` —— gateway_setup_trace 助手 | T1 |
| 11 | Gateway 改造示例：`GetMailVerifyCode` 接入 gateway_setup_trace | T10 |
| 12 | Gateway 机械改造：剩余 33 个 handler 套用同模式 | T11 |
| 13 | Transmite 改造：publish_confirm 时注入 trace_id headers | T6, T8 |
| 14 | Message 改造：MQ 消费起首 LogContext::set；下游 publish 透传 trace_id headers | T6, T8 |
| 15 | Push 改造：MQ 消费起首 LogContext::set；构造 NotifyMessage 填 trace_id | T6, T8, T9 |
| 16 | 文档：`docs/operations/log-format.md` | T5 |
| 17 | 文档：`docs/operations/log-levels.md` | 无 |
| 18 | 文档：`docs/operations/monitoring-conventions.md` | 无 |
| 19 | 文档：`docs/client-sdk/error-retry.md` | 无 |
| 20 | 端到端手动 smoke test runbook + Self-Review | 全部前置 |

每个 Task 自成 commit。所有 Task 完成后 P8 视为落地。

---

## Task 1: 新增 `common/utils/trace_id.hpp` —— 32 字符 hex 生成器

**Files:**
- Create: `common/utils/trace_id.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * trace_id —— 16 字节随机 → 32 字符小写 hex
 * ---
 * - 与 W3C Trace Context trace-id (16 字节) 兼容，便于未来接 OpenTelemetry
 * - 使用 std::random_device + std::mt19937_64 双 64 位拼成 128 位
 *   不直接调 /dev/urandom：std::random_device 在 glibc/Linux 实现内部已读
 *   /dev/urandom，跨平台同语义；mt19937 仅当 random_device 不可用时降级
 * - 输出全小写 hex，校验函数同样要求小写
 */

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace chatnow::utils {

inline std::string gen_trace_id() {
    // 拼 128 位
    std::random_device rd;
    static thread_local std::mt19937_64 prng_a(rd());
    static thread_local std::mt19937_64 prng_b(rd() ^ 0x9E3779B97F4A7C15ULL);
    uint64_t hi = prng_a();
    uint64_t lo = prng_b();

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(32);
    auto write_u64 = [&](uint64_t v, char* dst) {
        for (int i = 15; i >= 0; --i) {
            dst[i] = kHex[v & 0xF];
            v >>= 4;
        }
    };
    write_u64(hi, &out[0]);
    write_u64(lo, &out[16]);
    return out;
}

/* brief: 32 字符 + 全部为 [0-9a-f] */
inline bool is_valid_trace_id(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

}  // namespace chatnow::utils
```

- [ ] **Step 2: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "utils/trace_id.hpp"
#include <cassert>
int main() {
  auto t = chatnow::utils::gen_trace_id();
  assert(t.size() == 32);
  assert(chatnow::utils::is_valid_trace_id(t));
  return 0;
}' > /tmp/test_trace_id_compile.cc
g++ -std=c++17 -I common /tmp/test_trace_id_compile.cc -o /tmp/test_trace_id_compile && /tmp/test_trace_id_compile && echo OK
```

Expected: 输出 `OK`。

- [ ] **Step 3: Commit**

```bash
git add common/utils/trace_id.hpp
git commit -m "feat(common/utils): add gen_trace_id (32-char lowercase hex)"
```

---

## Task 2: 单元测试：trace_id 生成与校验

**Files:**
- Create: `common/test/test_trace_id.cc`

- [ ] **Step 1: 写测试**

```cpp
// common/test/test_trace_id.cc
#include "utils/trace_id.hpp"
#include <gtest/gtest.h>
#include <set>

using chatnow::utils::gen_trace_id;
using chatnow::utils::is_valid_trace_id;

TEST(TraceId, FormatLength32) {
    auto t = gen_trace_id();
    EXPECT_EQ(t.size(), 32u);
}

TEST(TraceId, AllHexLowercase) {
    auto t = gen_trace_id();
    for (char c : t) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(ok) << "non-hex char: " << c;
    }
}

TEST(TraceId, IsValidPositive) {
    EXPECT_TRUE(is_valid_trace_id("0123456789abcdef0123456789abcdef"));
    EXPECT_TRUE(is_valid_trace_id(gen_trace_id()));
}

TEST(TraceId, IsValidNegativeWrongLen) {
    EXPECT_FALSE(is_valid_trace_id(""));
    EXPECT_FALSE(is_valid_trace_id("abc"));
    EXPECT_FALSE(is_valid_trace_id(std::string(31, 'a')));
    EXPECT_FALSE(is_valid_trace_id(std::string(33, 'a')));
}

TEST(TraceId, IsValidNegativeUppercase) {
    EXPECT_FALSE(is_valid_trace_id("0123456789ABCDEF0123456789abcdef"));
}

TEST(TraceId, IsValidNegativeNonHex) {
    EXPECT_FALSE(is_valid_trace_id("0123456789abcdef0123456789abcdez"));
    EXPECT_FALSE(is_valid_trace_id("0123456789abcdef0123456789abcde "));
}

TEST(TraceId, UniquenessOver1000Calls) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(gen_trace_id());
    }
    // 概率上 1000 个 128bit 随机值碰撞接近 0
    EXPECT_EQ(seen.size(), 1000u);
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && cmake .. && make common_tests -j4 && ./common/test/common_tests --gtest_filter='TraceId*'
```

Expected: 7 tests passed.

- [ ] **Step 3: Commit**

```bash
git add common/test/test_trace_id.cc
git commit -m "test(common/utils): add tests for gen_trace_id (format/uniqueness/validation)"
```

---

## Task 3: 新增 `common/infra/log_json.hpp` —— JSON 转义 + build_log_line

**Files:**
- Create: `common/infra/log_json.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * log_json —— 自构 JSON 一行（不引第三方 JSON 库）
 * ---
 * 输出形如：
 *   {"ts":"2026-05-14T10:23:45.123Z","level":"info","service":"message",
 *    "trace_id":"...","user_id":"...","device_id":"...",
 *    "msg":"the message","fields":{"k":"v"}}
 * ---
 * 设计：
 *   - escape_json：仅处理必须转义字符（"、\、控制字符 < 0x20）；
 *     非 ASCII 原样保留（UTF-8 字节序列在 JSON 中合法）
 *   - build_log_line：把 ts/level/service/LogContext/msg/fields 拼接为单行 JSON
 *   - fields 类型：std::initializer_list<std::pair<std::string_view, std::string>>
 *     调用方需把数值/对象预先 to_string，避免泛型转 JSON 的复杂度
 *   - 时间戳：UTC ISO-8601 毫秒精度（与 spec §5.2 示例一致）
 */

#include "log/log_context.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace chatnow::infra {

/* brief: 写出 JSON-safe 字符串（不含外侧引号）；高频路径，预 reserve */
inline void escape_json_into(std::string& out, std::string_view s) {
    out.reserve(out.size() + s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

inline std::string escape_json(std::string_view s) {
    std::string out;
    escape_json_into(out, s);
    return out;
}

/* brief: 当前 UTC 毫秒 ISO-8601，例 "2026-05-14T10:23:45.123Z" */
inline std::string iso8601_utc_now_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    std::time_t secs = ms_total / 1000;
    int ms = static_cast<int>(ms_total % 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ms);
    return buf;
}

/* brief: 全局服务名（init 阶段调用 set_service_name 设定） */
inline std::string& mutable_service_name() {
    static std::string name = "unknown";
    return name;
}

inline void set_service_name(std::string name) {
    mutable_service_name() = std::move(name);
}

inline const std::string& service_name() {
    return mutable_service_name();
}

using FieldList = std::initializer_list<std::pair<std::string_view, std::string>>;

/* brief: 拼一行 JSON 日志
 *  level 推荐传 "trace"/"debug"/"info"/"warn"/"error"/"fatal" 全小写
 */
inline std::string build_log_line(std::string_view level,
                                  std::string_view msg,
                                  FieldList fields = {})
{
    const auto& ctx = ::chatnow::log::LogContext::current();
    std::string out;
    out.reserve(256 + msg.size());
    out += "{\"ts\":\"";
    out += iso8601_utc_now_ms();
    out += "\",\"level\":\"";
    escape_json_into(out, level);
    out += "\",\"service\":\"";
    escape_json_into(out, service_name());
    out += "\",\"trace_id\":\"";
    escape_json_into(out, ctx.trace_id);
    out += "\",\"user_id\":\"";
    escape_json_into(out, ctx.user_id);
    out += "\",\"device_id\":\"";
    escape_json_into(out, ctx.device_id);
    out += "\",\"msg\":\"";
    escape_json_into(out, msg);
    out += "\",\"fields\":{";
    bool first = true;
    for (const auto& kv : fields) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        escape_json_into(out, kv.first);
        out += "\":\"";
        escape_json_into(out, kv.second);
        out += "\"";
    }
    out += "}}";
    return out;
}

}  // namespace chatnow::infra
```

- [ ] **Step 2: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "infra/log_json.hpp"
#include <cassert>
#include <iostream>
int main() {
  auto s = chatnow::infra::build_log_line("info", "hello");
  std::cout << s << "\n";
  assert(s.find("\"level\":\"info\"") != std::string::npos);
  assert(s.find("\"msg\":\"hello\"") != std::string::npos);
  return 0;
}' > /tmp/test_log_json_compile.cc
g++ -std=c++17 -I common /tmp/test_log_json_compile.cc -o /tmp/test_log_json_compile && /tmp/test_log_json_compile && echo OK
```

Expected: 输出一行 JSON 与 `OK`。

- [ ] **Step 3: Commit**

```bash
git add common/infra/log_json.hpp
git commit -m "feat(common/infra): add log_json (escape + build_log_line + service_name)"
```

---

## Task 4: 单元测试：log_json 转义边界

**Files:**
- Create: `common/test/test_log_json.cc`

- [ ] **Step 1: 写测试**

```cpp
// common/test/test_log_json.cc
#include "infra/log_json.hpp"
#include "log/log_context.hpp"
#include <gtest/gtest.h>

using chatnow::infra::escape_json;
using chatnow::infra::build_log_line;
using chatnow::infra::set_service_name;
using chatnow::log::LogContext;

class LogJsonTest : public ::testing::Test {
protected:
    void SetUp() override { LogContext::clear(); set_service_name("testsvc"); }
    void TearDown() override { LogContext::clear(); }
};

TEST_F(LogJsonTest, EscapeQuoteAndBackslash) {
    EXPECT_EQ(escape_json("a\"b"),  "a\\\"b");
    EXPECT_EQ(escape_json("a\\b"),  "a\\\\b");
}

TEST_F(LogJsonTest, EscapeControlChars) {
    EXPECT_EQ(escape_json("a\nb"), "a\\nb");
    EXPECT_EQ(escape_json("a\tb"), "a\\tb");
    EXPECT_EQ(escape_json("a\rb"), "a\\rb");
    EXPECT_EQ(escape_json("a\bb"), "a\\bb");
    EXPECT_EQ(escape_json("a\fb"), "a\\fb");
}

TEST_F(LogJsonTest, EscapeOtherControlAsUnicode) {
    // 0x01 → 
    std::string in;
    in.push_back(static_cast<char>(0x01));
    EXPECT_EQ(escape_json(in), "\\u0001");
}

TEST_F(LogJsonTest, NonAsciiPassthrough) {
    // UTF-8 中文不应转义
    std::string s = "你好";
    EXPECT_EQ(escape_json(s), s);
}

TEST_F(LogJsonTest, BuildLineHasRequiredKeys) {
    LogContext::set("trace1", "u1", "d1");
    auto line = build_log_line("info", "msg1");
    EXPECT_NE(line.find("\"level\":\"info\""),       std::string::npos);
    EXPECT_NE(line.find("\"service\":\"testsvc\""),  std::string::npos);
    EXPECT_NE(line.find("\"trace_id\":\"trace1\""),  std::string::npos);
    EXPECT_NE(line.find("\"user_id\":\"u1\""),       std::string::npos);
    EXPECT_NE(line.find("\"device_id\":\"d1\""),     std::string::npos);
    EXPECT_NE(line.find("\"msg\":\"msg1\""),         std::string::npos);
    EXPECT_NE(line.find("\"fields\":{}"),            std::string::npos);
    EXPECT_NE(line.find("\"ts\":\""),                std::string::npos);
}

TEST_F(LogJsonTest, BuildLineFields) {
    auto line = build_log_line("warn", "evt", {{"k1","v1"}, {"k2","v2"}});
    EXPECT_NE(line.find("\"fields\":{\"k1\":\"v1\",\"k2\":\"v2\"}"),
              std::string::npos);
}

TEST_F(LogJsonTest, BuildLineEscapesMsgAndFieldValue) {
    auto line = build_log_line("info", "msg \"q\" \\back",
                               {{"k","v\nl2"}});
    EXPECT_NE(line.find("\"msg\":\"msg \\\"q\\\" \\\\back\""),
              std::string::npos);
    EXPECT_NE(line.find("\"k\":\"v\\nl2\""),
              std::string::npos);
}

TEST_F(LogJsonTest, BuildLineBalancedBraces) {
    // 简易"JSON 语法体检"：{ 与 } 数量平衡
    auto line = build_log_line("info", "x", {{"a","b"}});
    int open = 0, close = 0;
    for (char c : line) { if (c == '{') ++open; if (c == '}') ++close; }
    EXPECT_EQ(open, close);
    EXPECT_EQ(open, 2);  // 顶层 {} + fields {}
}

TEST_F(LogJsonTest, EmptyContextStillProducesKeys) {
    auto line = build_log_line("debug", "x");
    EXPECT_NE(line.find("\"trace_id\":\"\""),  std::string::npos);
    EXPECT_NE(line.find("\"user_id\":\"\""),   std::string::npos);
    EXPECT_NE(line.find("\"device_id\":\"\""), std::string::npos);
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 && ./common/test/common_tests --gtest_filter='LogJson*'
```

Expected: 9 tests passed.

- [ ] **Step 3: Commit**

```bash
git add common/test/test_log_json.cc
git commit -m "test(common/infra): add tests for log_json escape and build_log_line"
```

---

## Task 5: 改写 `common/infra/logger.hpp` —— pattern 切 `%v`、宏改 JSON、新增 LOG_xxxF

**Files:**
- Modify: `common/infra/logger.hpp`

**关键决策**：保留 `LOG_INFO/WARN/ERROR/DEBUG/TRACE/FATAL` 宏名与签名（`format, args...`）。宏体改为：先 `fmt::format` 拼出业务 msg → 调 `build_log_line(level_str, msg)` → 交给 `g_default_logger->log(level_enum, "{}", line)`。spdlog pattern 改 `"%v"`，等价于 sink 直接打印一行 JSON 不加额外装饰。

新增 `LOG_INFOF/WARNF/ERRORF/DEBUGF/TRACEF/FATALF`：第 1 参数是事件名（msg），第 2 参数是 `std::initializer_list` 字段。

- [ ] **Step 1: 完整覆盖文件**

```cpp
#pragma once

/**
 * ===========================================================================
 * 日志封装（基于 spdlog）—— P8 改造为结构化 JSON 输出
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. spdlog pattern 切为 "%v"：sink 仅打印 message 原文，不再叠加自己的
 *      时间戳/level/线程 ID。完整 JSON 由 LOG_xxx 宏侧组装。
 *   2. 宏侧自构 JSON：调用 build_log_line(level, msg, fields)，从 LogContext
 *      读 trace_id / user_id / device_id；服务名由 set_service_name 全局设
 *   3. 字段化变体：LOG_INFOF("event", {{"k","v"}}) —— 适合 spec §5.2 推荐用法
 *      传统宏 LOG_INFO("text {}", arg) 仍然工作，输出 fields 为空对象
 *   4. flush 策略不变：debug 模式即刷；release 模式 warn 以上即刷，余 3s 刷
 *   5. async 写盘 + rotating file 不变
 * ===========================================================================
 */

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/async.h>
#include <spdlog/fmt/fmt.h>
#include <chrono>
#include <memory>
#include <string>

#include "infra/log_json.hpp"   // build_log_line / set_service_name

namespace chatnow
{

inline std::shared_ptr<spdlog::logger> g_default_logger;

inline constexpr size_t kMaxFileSize  = 50 * 1024 * 1024;
inline constexpr size_t kMaxFileCount = 10;
inline constexpr size_t kAsyncQueue   = 8192;

/* brief: 设置当前进程的服务名（用于日志 service 字段）
 *  在 main() 早于 init_logger 调用，例如：
 *    chatnow::set_service_name("message");
 *    chatnow::init_logger(release_mode, "/var/log/chatnow/message.log", level);
 */
using ::chatnow::infra::set_service_name;

inline void init_logger(bool mode, const std::string &filename, int32_t level) {
    if(mode == false) {
        spdlog::drop("console-logger");
        g_default_logger = spdlog::stdout_color_mt("console-logger");
        g_default_logger->set_level(spdlog::level::trace);
        g_default_logger->flush_on(spdlog::level::trace);
    } else {
        spdlog::drop("file-logger");
        spdlog::init_thread_pool(kAsyncQueue, 1);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            filename, kMaxFileSize, kMaxFileCount);
        g_default_logger = std::make_shared<spdlog::async_logger>(
            "file-logger",
            sink,
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        spdlog::register_logger(g_default_logger);
        g_default_logger->set_level(static_cast<spdlog::level::level_enum>(level));
        g_default_logger->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(3));
    }
    /* P8: 把 sink 输出格式改为仅打印 message —— 业务侧已经组装好 JSON */
    g_default_logger->set_pattern("%v");
}

/* brief: 抽 basename，用于把文件名加进 fields（不再放在 msg 里）  */
constexpr const char* basename_of(const char* path) {
    const char* last = path;
    for(const char* p = path; *p; ++p) {
        if(*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

namespace detail {
/* brief: 把传统 LOG_xxx 的 fmt-style 参数拼成 msg 后交给 build_log_line */
template <typename... Args>
inline std::string format_text(const std::string& fmt_str, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return fmt_str;
    } else {
        return fmt::format(fmt_str, std::forward<Args>(args)...);
    }
}
}  // namespace detail

}  // namespace chatnow

/* ============================================================
 *  传统宏 —— 输出 JSON，msg 字段为格式化后文本，fields 为空
 *  注意：原宏在 msg 前缀加 "[file:line] "，P8 改为放进 fields
 *  仍保留 file/line 作为 fields，便于排障定位
 * ============================================================ */
#define _CN_LOG_JSON(level_enum, level_str, format, ...)                       \
    do {                                                                       \
        if (!chatnow::g_default_logger) break;                                 \
        std::string _msg = ::chatnow::detail::format_text(format, ##__VA_ARGS__); \
        std::string _line = ::chatnow::infra::build_log_line(                  \
            level_str, _msg,                                                   \
            {{"file", chatnow::basename_of(__FILE__)},                         \
             {"line", std::to_string(__LINE__)}});                             \
        chatnow::g_default_logger->log(level_enum, "{}", _line);               \
    } while (0)

#define LOG_TRACE(format, ...) _CN_LOG_JSON(spdlog::level::trace,    "trace", format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) _CN_LOG_JSON(spdlog::level::debug,    "debug", format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  _CN_LOG_JSON(spdlog::level::info,     "info",  format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  _CN_LOG_JSON(spdlog::level::warn,     "warn",  format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) _CN_LOG_JSON(spdlog::level::err,      "error", format, ##__VA_ARGS__)
#define LOG_FATAL(format, ...) _CN_LOG_JSON(spdlog::level::critical, "fatal", format, ##__VA_ARGS__)

/* ============================================================
 *  字段化变体 —— event 为 msg；fields 由调用方传 initializer_list
 *  用法： LOG_INFOF("consumed_db_message",
 *           {{"message_id", std::to_string(id)},
 *            {"latency_ms", std::to_string(t)}});
 *  注意：value 必须是 std::string 已字符串化的形态
 * ============================================================ */
#define _CN_LOG_JSON_F(level_enum, level_str, event, fields_init)              \
    do {                                                                       \
        if (!chatnow::g_default_logger) break;                                 \
        std::string _line = ::chatnow::infra::build_log_line(                  \
            level_str, event, fields_init);                                    \
        chatnow::g_default_logger->log(level_enum, "{}", _line);               \
    } while (0)

#define LOG_TRACEF(event, fields) _CN_LOG_JSON_F(spdlog::level::trace,    "trace", event, fields)
#define LOG_DEBUGF(event, fields) _CN_LOG_JSON_F(spdlog::level::debug,    "debug", event, fields)
#define LOG_INFOF(event,  fields) _CN_LOG_JSON_F(spdlog::level::info,     "info",  event, fields)
#define LOG_WARNF(event,  fields) _CN_LOG_JSON_F(spdlog::level::warn,     "warn",  event, fields)
#define LOG_ERRORF(event, fields) _CN_LOG_JSON_F(spdlog::level::err,      "error", event, fields)
#define LOG_FATALF(event, fields) _CN_LOG_JSON_F(spdlog::level::critical, "fatal", event, fields)
```

- [ ] **Step 2: 验证 syntax**

```bash
cd /Users/yanghaoyang/repo/ChatNow
echo '#include "infra/logger.hpp"
int main() {
  chatnow::init_logger(false, "", 0);
  chatnow::set_service_name("testsvc");
  LOG_INFO("hello {}", 42);
  LOG_INFOF("evt", {{"k","v"}});
  return 0;
}' > /tmp/test_logger_compile.cc
g++ -std=c++17 -I common -I /usr/local/include -I /usr/include -lspdlog -lfmt /tmp/test_logger_compile.cc -o /tmp/test_logger_compile 2>&1 | head -30
```

Expected: 编译通过；运行后可见两行 JSON 输出，第一行 fields 含 file/line + msg=`hello 42`，第二行 fields 为 `{"k":"v"}` + msg=`evt`。

- [ ] **Step 3: 增量构建检查（确保下游服务不破）**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j4 2>&1 | tail -40
```

Expected: 全部既有服务（gateway/message/transmite/push/...）编译通过。spdlog 宏内部对 fmt 字符串校验在编译期开启，而本宏改造把原始 format 通过 `fmt::format` 安全格式化，再以 `"{}"` 单参数喂给 spdlog，不会触发 fmt 编译期类型校验问题。

- [ ] **Step 4: Commit**

```bash
git add common/infra/logger.hpp
git commit -m "feat(common/infra): switch spdlog output to JSON; add LOG_xxxF field-style macros"
```

---

## Task 6: 新增 `common/mq/trace_headers.hpp` —— MQ 头部 trace_id 注入/提取

**Files:**
- Create: `common/mq/trace_headers.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * MQ trace headers 助手
 * ---
 * - 发布侧：从当前 LogContext 取 trace_id 注入 std::map<string,string>
 *   作为 publish 的 headers 入参
 * - 消费侧：从 std::map（rabbitmq.hpp 改造后回调暴露）中读 trace_id；
 *   缺失时返回空串
 *
 * 不直接依赖 AMQP-CPP 类型，便于单测与跨 MQ 实现复用。
 */

#include "log/log_context.hpp"
#include <map>
#include <string>

namespace chatnow::mq {

inline constexpr const char* kTraceHeader = "trace_id";

/* brief: 把当前 LogContext 的 trace_id 注入 headers map（若非空）
 *  使用：发布前调 mq_inject_trace_headers(headers); publisher.publish_confirm(body, headers, cb);
 */
inline void mq_inject_trace_headers(std::map<std::string, std::string>& headers) {
    const auto& trace_id = ::chatnow::log::LogContext::current().trace_id;
    if (!trace_id.empty()) {
        headers[kTraceHeader] = trace_id;
    }
}

/* brief: 从 headers map 中读 trace_id；缺失返回 "" */
inline std::string mq_extract_trace_id(const std::map<std::string, std::string>& headers) {
    auto it = headers.find(kTraceHeader);
    if (it == headers.end()) return "";
    return it->second;
}

}  // namespace chatnow::mq
```

- [ ] **Step 2: Commit**

```bash
git add common/mq/trace_headers.hpp
git commit -m "feat(common/mq): add trace_headers inject/extract helpers"
```

---

## Task 7: 单元测试：mq_trace_headers

**Files:**
- Create: `common/test/test_mq_trace_headers.cc`

- [ ] **Step 1: 写测试**

```cpp
// common/test/test_mq_trace_headers.cc
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
#include <gtest/gtest.h>

using namespace chatnow::mq;
using chatnow::log::LogContext;

class MqTraceHeadersTest : public ::testing::Test {
protected:
    void TearDown() override { LogContext::clear(); }
};

TEST_F(MqTraceHeadersTest, InjectFromLogContext) {
    LogContext::set("trace-abc", "u", "d");
    std::map<std::string, std::string> h;
    mq_inject_trace_headers(h);
    EXPECT_EQ(h[kTraceHeader], "trace-abc");
}

TEST_F(MqTraceHeadersTest, InjectSkipsWhenContextEmpty) {
    std::map<std::string, std::string> h;
    mq_inject_trace_headers(h);
    EXPECT_EQ(h.find(kTraceHeader), h.end());
}

TEST_F(MqTraceHeadersTest, InjectPreservesOtherHeaders) {
    LogContext::set("trace-xyz", "", "");
    std::map<std::string, std::string> h{{"existing", "val"}};
    mq_inject_trace_headers(h);
    EXPECT_EQ(h["existing"], "val");
    EXPECT_EQ(h[kTraceHeader], "trace-xyz");
}

TEST_F(MqTraceHeadersTest, ExtractPresent) {
    std::map<std::string, std::string> h{{kTraceHeader, "tt"}};
    EXPECT_EQ(mq_extract_trace_id(h), "tt");
}

TEST_F(MqTraceHeadersTest, ExtractMissingReturnsEmpty) {
    std::map<std::string, std::string> h;
    EXPECT_EQ(mq_extract_trace_id(h), "");
}

TEST_F(MqTraceHeadersTest, ExtractIgnoresOtherKeys) {
    std::map<std::string, std::string> h{{"foo", "bar"}};
    EXPECT_EQ(mq_extract_trace_id(h), "");
}
```

- [ ] **Step 2: 编译 + 运行**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make common_tests -j4 && ./common/test/common_tests --gtest_filter='MqTraceHeaders*'
```

Expected: 6 tests passed.

- [ ] **Step 3: Commit**

```bash
git add common/test/test_mq_trace_headers.cc
git commit -m "test(common/mq): add tests for mq trace headers inject/extract"
```

---

## Task 8: 改造 `common/mq/rabbitmq.hpp` —— publish 加 headers；consume 暴露 headers

**Files:**
- Modify: `common/mq/rabbitmq.hpp`

**改造要点**：
1. `MQClient::publish_confirm` 增加新签名（带 headers），旧签名保留为转发到新签名 + 空 headers。
2. `Publisher::publish_confirm` 增加带 headers 的重载；不破坏现有调用。
3. `MessageCallback` typedef 保留旧签名；新增 `MessageCallbackWithHeaders` 类型；`MQClient::consume` 重载支持二者。`Subscriber::consume` 同样新增重载。

- [ ] **Step 1: Edit 修改文件**

`MessageCallback` typedef 之后插入：

```cpp
using MessageCallbackWithHeaders =
    std::function<ConsumeAction(const char*, size_t, bool,
                                const std::map<std::string,std::string>&)>;
```

并在文件顶部 include 列表内新增 `#include <map>`。

`MQClient` class public 段，`publish_confirm`（带 callback 4 参版）之后追加新签名：

```cpp
/* brief: 带 broker 确认的发布；headers 进 AMQP envelope（用于 trace_id 等透传） */
void publish_confirm(const std::string &exchange,
                     const std::string &routing_key,
                     const std::string &body,
                     const std::map<std::string, std::string> &headers,
                     const PublishConfirmCallback &callback)
{
    if(!_reliable) {
        if(callback) callback(PublishStatus::Error, "发布确认未启用");
        return;
    }
    post_task([this, exchange, routing_key, body, headers, callback]() {
        AMQP::Envelope env(body.data(), body.size());
        for (const auto &kv : headers) {
            env.setHeader(kv.first, kv.second);
        }
        _reliable->publish(exchange, routing_key, env)
            .onAck  ([callback]()                  { if(callback) callback(PublishStatus::Acked,  "broker 已确认"); })
            .onNack ([callback]()                  { if(callback) callback(PublishStatus::Nacked, "broker 显式拒绝"); })
            .onLost ([callback]()                  { if(callback) callback(PublishStatus::Lost,   "通道断开，状态未知"); })
            .onError([callback](const char *msg)   { if(callback) callback(PublishStatus::Error, msg ? msg : "未知错误"); });
    });
}
```

旧 4 参 `publish_confirm` 实现保持不变（短期内多个调用方仍走旧路径；P8 内仅 transmite/message 改为新签名）。

`MQClient::consume` 之后新增重载（带 headers 回调）：

```cpp
bool consume(const std::string &queue, const MessageCallbackWithHeaders &callback,
             uint16_t prefetch = kDefaultPrefetch)
{
    std::promise<bool> promise;
    auto future = promise.get_future();

    post_task([this, queue, callback, prefetch, &promise]() {
        _channel.setQos(prefetch);
        _channel.consume(queue)
            .onMessage([this, callback](const AMQP::Message &message,
                                        uint64_t deliveryTag,
                                        bool redelivered) {
                std::map<std::string, std::string> headers;
                const auto &table = message.headers();
                for (const auto &kv : table) {
                    // AMQP::Field 仅在为字符串类型时取出；其他类型转为字符串表示
                    if (kv.second.isString()) {
                        headers[kv.first] = std::string(kv.second);
                    }
                }
                try {
                    ConsumeAction action = callback(message.body(), message.bodySize(),
                                                    redelivered, headers);
                    switch(action) {
                    case ConsumeAction::Ack:         _channel.ack(deliveryTag); break;
                    case ConsumeAction::NackRequeue: _channel.reject(deliveryTag, true);  break;
                    case ConsumeAction::NackDiscard: _channel.reject(deliveryTag, false); break;
                    }
                } catch(const std::exception &e) {
                    LOG_ERROR("消费回调异常: {}", e.what());
                    _channel.reject(deliveryTag, true);
                } catch(...) {
                    LOG_ERROR("消费回调发生未知异常");
                    _channel.reject(deliveryTag, true);
                }
            })
            .onError([&promise, queue](const char *message) {
                LOG_ERROR("订阅消息失败: {} - {}", queue, message ? message : "");
                promise.set_value(false);
            })
            .onSuccess([&promise, queue]() {
                LOG_DEBUG("成功订阅队列: {}", queue);
                promise.set_value(true);
            });
    });
    return future.get();
}
```

`Publisher` 增加重载：

```cpp
void publish_confirm(const std::string &body,
                     const std::map<std::string, std::string> &headers,
                     const PublishConfirmCallback &cb) {
    _mq->publish_confirm(_settings.exchange, _settings.binding_key, body, headers, cb);
}
```

`Subscriber` 增加重载：

```cpp
void consume(MessageCallbackWithHeaders &&cb, uint16_t prefetch = kDefaultPrefetch) {
    const std::string &q = (_settings.exchange_type == DELAYED)
        ? _settings.dlx_queue() : _settings.queue;
    _mq->consume(q, cb, prefetch);
}
```

- [ ] **Step 2: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && cmake .. && make -j4 2>&1 | tail -40
```

Expected: gateway/message/transmite/push 全部编译通过。新增重载不冲突现有调用（旧调用走旧无 headers 路径）。

- [ ] **Step 3: Commit**

```bash
git add common/mq/rabbitmq.hpp
git commit -m "feat(common/mq): support AMQP headers in publish_confirm and consume"
```

---

## Task 9: 修改 `proto/push/notify.proto` —— `NotifyMessage.trace_id = 14`

**Files:**
- Modify: `proto/push/notify.proto`

- [ ] **Step 1: Edit**

把 `NotifyMessage` 改为：

```protobuf
message NotifyMessage {
    optional string notify_event_id = 1;
    NotifyType notify_type = 2;
    optional string trace_id = 14;             // P8: 推送链路 trace_id 透传
    oneof notify_remarks {
        NotifyFriendAddApply friend_add_apply = 3;
        NotifyFriendAddProcess friend_process_result = 4;
        NotifyFriendRemove friend_remove = 7;
        NotifyNewConversation new_conversation_info = 5;
        NotifyNewMessage new_message_info = 6;
        NotifyMsgPushAck msg_push_ack = 8;
        NotifyHeartbeat heartbeat = 9;
        NotifyClientAuth client_auth = 10;
        NotifyMessageRecalled message_recalled = 11;
        NotifyPresenceChange presence_change = 12;
        NotifyTyping typing = 13;
    }
}
```

- [ ] **Step 2: 验证 proto 编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow
protoc --cpp_out=/tmp -I proto --experimental_allow_proto3_optional proto/push/notify.proto
grep "trace_id" /tmp/push/notify.pb.h | head -5
```

Expected: 看到 `trace_id` 相关 setter/getter；无报错。

- [ ] **Step 3: 增量构建**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j4 2>&1 | tail -20
```

Expected: 编译通过（trace_id 是 optional 新字段，老序列化反序列化向后兼容）。

- [ ] **Step 4: Commit**

```bash
git add proto/push/notify.proto
git commit -m "feat(proto/push): add NotifyMessage.trace_id (field 14) for client correlation"
```

---

## Task 10: 新增 `gateway/source/gateway_trace.hpp` —— gateway_setup_trace 助手

**Files:**
- Create: `gateway/source/gateway_trace.hpp`

- [ ] **Step 1: 写入文件**

```cpp
#pragma once

/**
 * gateway_setup_trace
 * ---
 * Gateway 每个 HTTP handler 入口三件套：
 *   1. 从 HTTP 请求读 X-Trace-Id；不合法则现生成 32 字符 hex
 *   2. 写到 brpc Controller 的 HTTP header（key 为 x-trace-id）
 *      传给后端 RPC；后端 extract_auth() 从 metadata 取出来
 *   3. LogContext::set(trace_id, user_id, device_id)
 *      让 Gateway 自身的 LOG_xxx 输出也带 trace_id
 *
 * user_id / device_id 在 P2 JWT 实施前可填空串；P2 之后改为 JWT payload 取值。
 *
 * Handler 退出时调 LogContext::clear()——本助手不接管，handler 用 RAII 守卫。
 */

#include "auth/metadata_keys.hpp"
#include "log/log_context.hpp"
#include "utils/trace_id.hpp"

#include <brpc/controller.h>
#include <httplib.h>
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

/* brief: 一行接入：解析 trace_id → 写 cntl metadata → 写 LogContext
 *   返回 trace_id（调用方按需用，例如填回 HTTP response header 给客户端）
 *   user_id/device_id 为空字符串时不写入 LogContext 字段（但仍会被 set 覆盖
 *   为空，因此调用方应保证一次 handler 内只调一次本函数）
 */
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       brpc::Controller& cntl,
                                       const std::string& user_id = "",
                                       const std::string& device_id = "")
{
    std::string trace_id = resolve_trace_id(req);
    cntl.http_request().SetHeader(::chatnow::auth::kMetaTraceId, trace_id);
    if (!user_id.empty()) {
        cntl.http_request().SetHeader(::chatnow::auth::kMetaUserId, user_id);
    }
    if (!device_id.empty()) {
        cntl.http_request().SetHeader(::chatnow::auth::kMetaDeviceId, device_id);
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

- [ ] **Step 2: 验证编译**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j4 2>&1 | tail -20
```

Expected: 编译通过。

- [ ] **Step 3: Commit**

```bash
git add gateway/source/gateway_trace.hpp
git commit -m "feat(gateway): add gateway_setup_trace helper for HTTP handlers"
```

---

## Task 11: Gateway 改造示例：`GetMailVerifyCode` 接入 gateway_setup_trace

**Files:**
- Modify: `gateway/source/gateway_server.h`

- [ ] **Step 1: 加 include**

在文件顶部 include 区追加：

```cpp
#include "gateway_trace.hpp"
```

- [ ] **Step 2: 修改 `GetMailVerifyCode` handler**

把当前 handler（约 line 213-238）改为：

```cpp
void GetMailVerifyCode(const httplib::Request &request, httplib::Response &response) {
    chatnow::gateway::LogContextScope _trace_scope;
    GetMailVerifyCodeReq req;
    GetMailVerifyCodeRsp rsp;
    auto err_response = [&req, &rsp, &response](const std::string &errmsg) -> void {
        rsp.set_success(false);
        rsp.set_errmsg(errmsg);
        response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
    };
    bool ret = req.ParseFromString(request.body);
    if(ret == false) {
        LOG_ERROR("获取邮件验证码请求正文反序列化失败");
        return err_response("获取邮件验证码请求正文反序列化失败");
    }
    auto channel = _mm_channels->choose(_user_service_name);
    if(!channel) {
        LOG_ERROR("请求ID - {} 未找到可提供业务的用户子服务节点", req.request_id());
        return err_response("未找到可提供业务的用户子服务节点");
    }
    UserService_Stub stub(channel.get());
    brpc::Controller cntl;
    /* P8: 入口三件套 —— trace_id 解析/生成 + 写 metadata + LogContext::set */
    std::string trace_id = chatnow::gateway::gateway_setup_trace(request, cntl);
    response.set_header("X-Trace-Id", trace_id);   // 把 trace_id 回传给客户端日志
    stub.GetMailVerifyCode(&cntl, &req, &rsp, nullptr);
    if(cntl.Failed()) {
        LOG_ERROR("请求ID - {} 用户子服务调用失败: {}", req.request_id(), cntl.ErrorText());
        return err_response("用户子服务调用失败");
    }
    response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
}
```

3 处变化：
1. handler 第一行加 `chatnow::gateway::LogContextScope _trace_scope;`（RAII，函数退出 clear LogContext）
2. `brpc::Controller cntl;` 之后加 `std::string trace_id = gateway_setup_trace(request, cntl);` 与 `response.set_header("X-Trace-Id", trace_id);`

- [ ] **Step 3: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j4 2>&1 | tail -20
```

Expected: 编译通过。

- [ ] **Step 4: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "feat(gateway): wire trace_id into GetMailVerifyCode (template handler)"
```

---

## Task 12: Gateway 机械改造：剩余 33 个 handler 套用同模式

**Files:**
- Modify: `gateway/source/gateway_server.h`（约 33 处）

**改造规则**（机械化，逐 handler 应用 3 处变化）：

每个 handler 按以下顺序找到 3 个锚点并按规则插入：

1. **锚点 A**：handler 函数体打开 `{` 之后的第 1 行（即第一条声明语句之前）。
   - **插入**：`chatnow::gateway::LogContextScope _trace_scope;`
2. **锚点 B**：`brpc::Controller cntl;` 行之后。
   - **插入**：
     ```cpp
     std::string trace_id = chatnow::gateway::gateway_setup_trace(request, cntl);
     response.set_header("X-Trace-Id", trace_id);
     ```
3. **锚点 C**：无变化（已存在的代码不动）。

**Handler 列表**（参考 `gateway_server.h` line 119-167 的 `_http_server.Post(...)` 注册表）。除 Task 11 已完成的 `GetMailVerifyCode` 外，剩余 33 个为：

```
UserRegister                MailRegister              UserLogin
MailLogin                   GetUserInfo               SetUserAvatar
SetUserNickname             SetUserDescription        SetUserMail
GetFriendList               GetFriendInfo             AddFriendApply
AddFriendProcess            RemoveFriend              SearchFriend
GetChatSessionList          ChatSessionCreate         GetChatSessionMember
GetPendingFriendEvents      GetHistory                GetRecent
SearchHistory               NewMessage                GetSingleFile
GetMultiFile                PutSingleFile             PutMultiFile
SpeechRecognition           GetChatSessionDetail      SetChatSessionName
SetChatSessionAvatar        AddChatSessionMember      RemoveChatSessionMember
TransferChatSessionOwner    ModifyMemberPermission    ModifyChatSessionStatus
SearchChatSession           SetSessionMuted           SetSessionPinned
SetSessionVisible           GetUserSessionStatus      QuitChatSession
MsgReadAck                  GetMemberIdList           GetOfflineMsg
GetUnreadCount
```

(实际数量以 `_http_server.Post(...)` 行为准；以下 search/replace 是按机器视角操作，无需人工逐一阅读 handler 实现。)

- [ ] **Step 1: 用 awk 批量改写**

在仓库根：

```bash
python3 - <<'PY'
import re, pathlib
p = pathlib.Path("gateway/source/gateway_server.h")
src = p.read_text()

# 锚点 A：每个 "void <Name>(const httplib::Request &request, httplib::Response &response) {"
# 紧随其后的换行处插入 LogContextScope。
# 已在 Task 11 修改过 GetMailVerifyCode；其首行已存在 _trace_scope 变量名 → 利用此 sentinel 跳过。
def insert_after_signature(m):
    sig = m.group(0)
    return sig + "\n        chatnow::gateway::LogContextScope _trace_scope;"
sig_re = re.compile(
    r'void\s+\w+\(const httplib::Request &request, httplib::Response &response\)\s*\{')
new_src_a_chunks = []
i = 0
for m in sig_re.finditer(src):
    new_src_a_chunks.append(src[i:m.end()])
    # 跳过 Task 11 已改造的那个：在该 handler 首行查找 _trace_scope
    body_start = m.end()
    body_first_line_end = src.find('\n', body_start)
    first_line = src[body_start:body_first_line_end]
    second_line_end = src.find('\n', body_first_line_end + 1)
    second_line = src[body_first_line_end+1:second_line_end]
    if "_trace_scope" in (first_line + second_line):
        i = m.end()
        continue
    # 否则插入
    new_src_a_chunks.append("\n        chatnow::gateway::LogContextScope _trace_scope;")
    i = m.end()
new_src_a_chunks.append(src[i:])
src = "".join(new_src_a_chunks)

# 锚点 B：每处 "brpc::Controller cntl;" 后插入 setup_trace
# 已改过的（GetMailVerifyCode）已存在 gateway_setup_trace 紧随其后 → sentinel 跳过
out_lines = []
lines = src.split('\n')
i = 0
while i < len(lines):
    out_lines.append(lines[i])
    stripped = lines[i].strip()
    if stripped == "brpc::Controller cntl;":
        # 检查后续 2 行是否已含 gateway_setup_trace（Task 11 sentinel）
        nxt = lines[i+1] if i+1 < len(lines) else ""
        if "gateway_setup_trace" not in nxt:
            indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
            out_lines.append(f'{indent}std::string trace_id = chatnow::gateway::gateway_setup_trace(request, cntl);')
            out_lines.append(f'{indent}response.set_header("X-Trace-Id", trace_id);')
    i += 1

p.write_text('\n'.join(out_lines))
print("done")
PY
```

- [ ] **Step 2: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make gateway -j4 2>&1 | tail -40
```

Expected: 编译通过。如果有 handler 没有 `brpc::Controller cntl;` 的标准模板（极少数特殊 handler），编译错误会指出位置；按 Task 11 模式手工补齐即可。

- [ ] **Step 3: 抽查 grep**

```bash
grep -c "gateway_setup_trace" /Users/yanghaoyang/repo/ChatNow/gateway/source/gateway_server.h
grep -c "LogContextScope _trace_scope" /Users/yanghaoyang/repo/ChatNow/gateway/source/gateway_server.h
```

Expected: 两个数字都 ≥ 34（含 Task 11 已改的 1 个）。如果数量不一致，定位漏改 handler 手工补齐。

- [ ] **Step 4: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "feat(gateway): apply trace_id setup to all remaining HTTP handlers"
```

---

## Task 13: Transmite 改造：publish_confirm 时注入 trace_id headers

**Files:**
- Modify: `transmite/source/transmite_server.h`

- [ ] **Step 1: 加 include**

在 transmite_server.h 顶部 include 区加：

```cpp
#include "mq/trace_headers.hpp"
```

- [ ] **Step 2: 修改 publish_confirm 调用点（约 line 264）**

把：

```cpp
_publisher->publish_confirm(internal_msg.SerializeAsString(),
    [...](PublishStatus status, const std::string& reason){ ... });
```

改为：

```cpp
std::map<std::string, std::string> mq_headers;
chatnow::mq::mq_inject_trace_headers(mq_headers);
_publisher->publish_confirm(internal_msg.SerializeAsString(),
                            mq_headers,
    [...](PublishStatus status, const std::string& reason){ ... });
```

注意 `internal_msg` 是 protobuf 序列化结果。HANDLE_RPC 宏已经在 RPC 入口设置了 LogContext（P1），所以 `mq_inject_trace_headers` 自动从 thread_local 取出当前 RPC 的 trace_id。

- [ ] **Step 3: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make transmite -j4 2>&1 | tail -20
```

Expected: 编译通过。

- [ ] **Step 4: Commit**

```bash
git add transmite/source/transmite_server.h
git commit -m "feat(transmite): inject trace_id into MQ headers when publishing"
```

---

## Task 14: Message 改造：MQ 消费起首 LogContext::set；下游 publish 透传 trace_id headers

**Files:**
- Modify: `message/source/message_server.h`

- [ ] **Step 1: 加 include**

```cpp
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
```

- [ ] **Step 2: 改 MQ 消费回调签名**

定位 message_server.h 中处理 transmite → message MQ 消费的 callback（消费 InternalMessage）。把回调改为新签名 `(const char* body, size_t size, bool redelivered, const std::map<std::string,std::string>& headers)`，并在订阅处用 `Subscriber::consume(MessageCallbackWithHeaders&&)` 重载。

回调起首加：

```cpp
std::string trace_id = chatnow::mq::mq_extract_trace_id(headers);
chatnow::log::LogContext::set(trace_id, /*user_id*/ "", /*device_id*/ "");
struct LogScope { ~LogScope() { chatnow::log::LogContext::clear(); } } _scope;
```

`user_id/device_id` 留空：MQ 消费不带用户上下文（消息体 InternalMessage 内部含 sender_id 等业务字段，但日志层不需要重复）。如果业务需要展示发送者，可在反序列化后再 `LogContext::set(trace_id, sender_id, "")` 覆盖。

- [ ] **Step 3: 改下游 publish（push / es）注入 headers**

定位 `_push_publisher->publish_confirm(...)` 与 `_es_publisher->publish_confirm(...)` 调用点（约 line 690、710）。每处改为：

```cpp
std::map<std::string, std::string> hdrs;
chatnow::mq::mq_inject_trace_headers(hdrs);
_es_publisher->publish_confirm(es_payload, hdrs, [...](...){ ... });
```

```cpp
std::map<std::string, std::string> hdrs;
chatnow::mq::mq_inject_trace_headers(hdrs);
_push_publisher->publish_confirm(payload, hdrs, [...](...){ ... });
```

由于 LogContext 已被消费回调起首设置好，`mq_inject_trace_headers` 自动透传同一 trace_id。

- [ ] **Step 4: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make message -j4 2>&1 | tail -30
```

Expected: 编译通过。

- [ ] **Step 5: Commit**

```bash
git add message/source/message_server.h
git commit -m "feat(message): MQ consume sets LogContext; downstream publish forwards trace_id headers"
```

---

## Task 15: Push 改造：MQ 消费起首 LogContext::set；NotifyMessage 填 trace_id

**Files:**
- Modify: `push/source/push_server.h`（或 push 主消费回调所在文件——以 grep 确认）

- [ ] **Step 1: 定位 MQ 消费入口**

```bash
grep -rn "consume\|onMessage\|Subscriber" /Users/yanghaoyang/repo/ChatNow/push/source/ | head -20
```

确认 push 服务消费 message → push 队列的回调位置。

- [ ] **Step 2: 加 include**

```cpp
#include "mq/trace_headers.hpp"
#include "log/log_context.hpp"
```

- [ ] **Step 3: 改回调签名 + 起首设置 LogContext**

把 push 服务的 message → push MQ 消费回调改为带 headers 签名：

```cpp
auto cb = [this](const char* body, size_t size, bool redeliv,
                 const std::map<std::string,std::string>& headers) -> ConsumeAction {
    std::string trace_id = chatnow::mq::mq_extract_trace_id(headers);
    chatnow::log::LogContext::set(trace_id, "", "");
    struct LogScope { ~LogScope() { chatnow::log::LogContext::clear(); } } _scope;

    // ... 原有反序列化与构造 NotifyMessage 的代码
    // P8: 把 trace_id 写入 NotifyMessage（下发到 WS 客户端）
    notify.set_trace_id(trace_id);
    // ... 其余业务
    return ConsumeAction::Ack;
};
```

- [ ] **Step 4: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make push -j4 2>&1 | tail -30
```

Expected: 编译通过。`notify.set_trace_id(...)` 由 Task 9 新增的 proto 字段提供。

- [ ] **Step 5: Commit**

```bash
git add push/source/push_server.h
git commit -m "feat(push): set LogContext from MQ headers; emit trace_id in NotifyMessage"
```

---

## Task 16: 文档：`docs/operations/log-format.md`

**Files:**
- Create: `docs/operations/log-format.md`

- [ ] **Step 1: 写文档**

```markdown
# 日志格式（结构化 JSON）

> P8 起 ChatNow 所有服务的日志输出为单行 JSON。本文记录 schema 与常用查询。

## Schema

每行日志是一个 JSON 对象，字段如下：

| 字段 | 类型 | 说明 |
|---|---|---|
| `ts` | string | UTC ISO-8601 毫秒精度，例 `2026-05-14T10:23:45.123Z` |
| `level` | string | `trace` / `debug` / `info` / `warn` / `error` / `fatal` |
| `service` | string | 进程启动时 `chatnow::set_service_name("...")` 设定，例 `gateway` / `message` / `push` |
| `trace_id` | string | 32 字符小写 hex；Gateway 入口生成；可能为空（如启动期日志） |
| `user_id` | string | 来自 LogContext；可能为空（无 RPC 上下文时） |
| `device_id` | string | 来自 LogContext；可能为空 |
| `msg` | string | 业务消息文本（传统 LOG_xxx）或事件名（LOG_xxxF） |
| `fields` | object | 字符串到字符串的扁平 map；调用方预序列化所有值 |

## 示例

```json
{"ts":"2026-05-14T10:23:45.123Z","level":"info","service":"message","trace_id":"a1b2c3d4e5f60718a1b2c3d4e5f60718","user_id":"u_42","device_id":"d_iphone","msg":"consumed_db_message","fields":{"message_id":"9876","conversation_id":"c_001","latency_ms":"12"}}
```

## 调用方约定

- **传统宏**：`LOG_INFO("user logged in: {}", uid);` —— `msg` 字段值为 `user logged in: 42`，`fields` 含 `file` / `line`。适合"自由文本调试日志"。
- **字段化宏**：`LOG_INFOF("user_login", {{"user_id", uid}, {"latency_ms", std::to_string(t)}});` —— `msg` 字段值为事件名 `user_login`，`fields` 为传入的 map。**生产关键路径请用此风格**，便于 ELK/Loki 索引。

## 常用 jq 查询

```bash
# 按 trace_id 串联整条链路
cat /var/log/chatnow/*.log | jq -c 'select(.trace_id == "a1b2c3d4e5f60718a1b2c3d4e5f60718")'

# 抽 ERROR 级日志按服务分组
cat /var/log/chatnow/*.log | jq -c 'select(.level == "error") | {service, msg, fields}'

# 某用户最近的 WARN
cat /var/log/chatnow/gateway.log | jq -c 'select(.level == "warn" and .user_id == "u_42")' | tail -20
```

## ELK / Loki 索引建议

- 主索引字段：`service`, `level`, `trace_id`, `user_id`
- 高基数字段（不索引仅存储）：`fields.message_id`, `fields.conversation_id`
- 时间字段：`ts`（解析为 timestamp）
```

- [ ] **Step 2: Commit**

```bash
git add docs/operations/log-format.md
git commit -m "docs(operations): document JSON log format and query patterns"
```

---

## Task 17: 文档：`docs/operations/log-levels.md`

**Files:**
- Create: `docs/operations/log-levels.md`

- [ ] **Step 1: 写文档**

```markdown
# 日志级别约定

> 来源：`docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §5.2

| 级别 | 用法 |
|---|---|
| `error` | 影响业务结果的失败：DB 写失败、MQ 投递失败、外部依赖（Redis/MinIO）报错、未捕获异常 |
| `warn`  | 客户端错误：鉴权失败、参数非法、配额超限；服务降级触发；MQ 重投 |
| `info`  | RPC 入口/出口（仅 Gateway 入口 + 服务边界）；MQ 消费成功；服务启动 / 配置加载 |
| `debug` | 默认关闭，本地或问题排查时按服务级开启 |
| `trace` | 极细粒度调试；生产环境永远关闭 |
| `fatal` | 启动期 fail-fast；密钥/配置缺失等无法恢复的错误 |

## 反模式（禁止）

- `LOG_INFO("step 1")` 等散落调试 INFO —— 改为 DEBUG，或加结构化字段说明步骤含义
- `LOG_ERROR(rsp.errmsg())` 缺上下文 —— 至少补上 trace_id（已由 LogContext 自动）+ key 业务字段（user_id, conv_id, message_id）
- 把 SQL 错误 / 栈信息 / 内部 IP / 文件路径写到 `error_message` 返回客户端 —— 改写日志层 ERROR + 客户端返回 "internal error"

## 关键路径推荐

- 用 `LOG_INFOF` / `LOG_WARNF` / `LOG_ERRORF` 字段化宏；事件名采用 `snake_case`，例：
  - `rpc_failed`, `rpc_exception`（HANDLE_RPC 宏内部）
  - `mq_publish_lost`, `mq_consume_dlq`
  - `auth_token_invalid`, `quota_exceeded`
- 不在 `info` 级别打印 PII（手机号、邮箱、设备 IP）；如必须打 `debug` 级别 + 字段加 `_redacted` 后缀
```

- [ ] **Step 2: Commit**

```bash
git add docs/operations/log-levels.md
git commit -m "docs(operations): document log level conventions"
```

---

## Task 18: 文档：`docs/operations/monitoring-conventions.md`

**Files:**
- Create: `docs/operations/monitoring-conventions.md`

- [ ] **Step 1: 写文档**

```markdown
# 监控告警约定（仅文档，不在代码层实施）

> 来源：`docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §5.8
> 实施侧：运维（ELK / Loki + 告警规则）

ChatNow 不引入 Prometheus client / OpenTelemetry collector；ERROR 级 JSON 日志即告警源。

## 告警规则建议

| 规则 | 阈值 | 告警动作 |
|---|---|---|
| ERROR 风暴 | 任一服务 1 分钟内 `level=error` 行数 > 100 | P2 通知值班 |
| 内部错误占比 | `error_code in {9001, 9002}` 占比 > 1%（5 分钟滚动窗） | P2 通知值班 |
| 服务静默 | 任一服务 3 分钟内无 `level=info` 行（推断进程挂） | P1 唤醒值班 |
| trace_id 链路断裂 | Gateway 一条 trace_id 在 Message 服务找不到对应日志（>5 分钟） | P3 排障 |
| Push DLQ 堆积 | `mq_consume_dlq` 事件 5 分钟 > 50 | P2 通知 |

阈值按上线后真实流量调参；以上为初始值。

## SLO（运维参考）

- Gateway HTTP 5xx 率 < 0.1%
- 消息端到端延迟 P99 < 500ms（客户端发出 → 接收方 WS 收到）
- ERROR 日志 / INFO 日志比 < 0.01%

## 实施提示

ELK 侧建议把 `level`, `service`, `trace_id`, `error_code`（若进入 fields）建成 keyword 索引；按上述规则在 Watcher / ElastAlert / Loki Ruler 中配置告警。
```

- [ ] **Step 2: Commit**

```bash
git add docs/operations/monitoring-conventions.md
git commit -m "docs(operations): add monitoring/alerting conventions (spec 5.8)"
```

---

## Task 19: 文档：`docs/client-sdk/error-retry.md`

**Files:**
- Create: `docs/client-sdk/error-retry.md`

- [ ] **Step 1: 创建目录 + 写文档**

```bash
mkdir -p /Users/yanghaoyang/repo/ChatNow/docs/client-sdk
```

```markdown
# 客户端重试策略约定

> 来源：`docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §5.7
> 适用：iOS / Android / Web / Desktop SDK 实现者

服务端按 `chatnow.common.ErrorCode` enum 返回错误码；客户端按 code 段决定重试与 UI 行为。

## 策略表

| ErrorCode 段 | 含义 | 客户端动作 |
|---|---|---|
| `1xxx` 认证 | `AUTH_TOKEN_EXPIRED`(1002) → 自动 RefreshToken 后重试请求 1 次；其余跳登录页 | 不普通重试 |
| `2xxx` 关系 | `ALREADY_FRIENDS` 等业务态 | 不重试；UI 按 code 自映射文案 |
| `3xxx` 会话 | `NOT_FOUND` / `NO_PERMISSION` | 不重试；UI 提示 |
| `4xxx` 消息 | `RECALL_TIMEOUT` 等 | 不重试 |
| `5xxx` 媒体 | `QUOTA_EXCEEDED` / `UNSUPPORTED_FORMAT` | 不重试 |
| `6xxx` Presence | `USER_OFFLINE` | 不重试 |
| `7xxx` Device | `LIMIT_EXCEEDED` | 不重试；引导用户管理设备 |
| `8001` 限流 | `RATE_LIMIT_EXCEEDED` | 退避重试：指数退避（500ms 起，2x，最多 3 次）+ 100~300ms 抖动 |
| `9001` 内部错误 | `SYSTEM_INTERNAL_ERROR` | 不重试；UI 提示"系统繁忙稍后再试" |
| `9002` 不可用 | `SYSTEM_UNAVAILABLE` | 退避重试，连续 3 次失败 → "服务暂时不可用" |
| `9003` 超时 | `SYSTEM_TIMEOUT` | 同 9002 |

## 实现要点

1. **AUTH_TOKEN_EXPIRED 自动刷新**：拦截器层捕获 1002，调 `RefreshToken`，新 access_token 写本地存储，重发原请求 1 次（仅 1 次，避免无限循环）。
2. **AUTH_REFRESH_TOKEN_REUSED (1008)**：refresh 重放检测命中 → 该设备已被攻击者使用，**立即清空本地 token + 跳登录页**，不要尝试再 refresh。
3. **退避抖动**：`sleep_ms = base * 2^attempt + rand(0, 300)`，最大 attempt = 3。
4. **不要在 UI 直接展示 `error_message`**：`error_message` 是后端写给开发者的，按 `error_code` 在客户端做文案映射（i18n）。

## trace_id

- 客户端可在请求 HTTP header 加 `X-Trace-Id: <32-char-hex>` 由服务端透传；不送则服务端自动生成。
- 服务端响应 header 含 `X-Trace-Id`；建议客户端日志记录此值，便于报障时与服务端日志关联。
- WS 推送的 `NotifyMessage.trace_id` 字段同样可用于客户端本地日志追踪。
```

- [ ] **Step 2: Commit**

```bash
git add docs/client-sdk/error-retry.md
git commit -m "docs(client-sdk): document error code retry policy (spec 5.7)"
```

---

## Task 20: 端到端手动 smoke test runbook + Self-Review

**Files:**
- 无（仅文档化测试步骤）

- [ ] **Step 1: 启动全栈环境**

```bash
cd /Users/yanghaoyang/repo/ChatNow
# 按现有 docker-compose / depends.sh 文档启动 etcd / redis / mysql / rabbitmq / minio / es
# 然后逐个启动后端服务（gateway / identity / conversation / message / push / transmite / es / media）
# 假设服务已在 release 模式跑，日志输出到 /var/log/chatnow/<service>.log
```

- [ ] **Step 2: 客户端发一条消息触发整条链路**

```bash
# 客户端 SDK / 测试脚本调 POST /service/message_transmit/new_message
# 带 X-Trace-Id: deadbeefcafebabedeadbeefcafebabe
# 也可省略 X-Trace-Id 由 Gateway 生成；以下示例假设客户端送
curl -X POST http://localhost:9000/service/message_transmit/new_message \
  -H "X-Trace-Id: deadbeefcafebabedeadbeefcafebabe" \
  --data-binary @new_message_req.bin
```

- [ ] **Step 3: 抓 7 个服务日志**

```bash
TID=deadbeefcafebabedeadbeefcafebabe
for svc in gateway identity transmite message push media conversation; do
    echo "--- $svc ---"
    grep -F "\"trace_id\":\"$TID\"" /var/log/chatnow/${svc}.log | head -5
done
```

Expected: 至少 5 个服务（gateway → transmite → message → push）在日志里有该 trace_id 的行；每个服务有 ≥1 条 INFO 日志（RPC 入口/出口）+ 业务级日志。

**关键验证点**：
- Gateway 日志中 service 字段为 `gateway`，其他服务对应正确
- trace_id 字段 = 客户端传入的 32 字符
- LogContext 字段 user_id/device_id 在 P2 JWT 完成前为空（P2 之后填充）
- `fields` 对象字段值全部为字符串

- [ ] **Step 4: 验证 WS 推送的 NotifyMessage.trace_id**

客户端 SDK 解码 push WS 帧后，应在 `NotifyMessage.trace_id` 字段看到同样的 32 字符 hex。建议客户端日志输出包含该字段，便于和服务端串联。

- [ ] **Step 5: Self-Review 走查**

打开本 plan 的 `Spec 覆盖检查` 段（下方）逐项核对。

- [ ] **Step 6: Commit（runbook）**

无新文件。

---

## 完成验收

P8 完成后应满足：

- [x] `common/utils/trace_id.hpp` 提供 `gen_trace_id()` / `is_valid_trace_id()` 且单测通过
- [x] `common/infra/log_json.hpp` 提供 JSON 日志拼装且单测通过
- [x] `common/infra/logger.hpp` 输出 JSON；保留所有 `LOG_xxx` 宏；新增 `LOG_xxxF` 字段化变体；`set_service_name` 全局设定服务名
- [x] `common/mq/rabbitmq.hpp` 支持 publish_confirm 带 headers + consume 带 headers 回调
- [x] `common/mq/trace_headers.hpp` 助手 + 单测
- [x] `proto/push/notify.proto` `NotifyMessage` 含 `trace_id = 14`
- [x] Gateway 全部 34 个 HTTP handler 入口三件套（gateway_setup_trace + LogContextScope）
- [x] Transmite / Message / Push 三个服务的 MQ 路径透传 trace_id headers 并在消费侧 LogContext::set
- [x] 4 篇文档（log-format / log-levels / monitoring-conventions / error-retry）落地
- [x] 端到端手动 smoke test 跑通：单条 trace_id 串联 ≥5 个服务日志
- [x] 现有 P1-P7 代码编译不破坏；现有 LOG_INFO/WARN/ERROR/DEBUG 调用全部继续工作

---

## 自审

**Spec 覆盖检查**（spec §5.1 / §5.2 / §5.7 / §5.8）：

- [x] §5.1 trace_id 生成（Gateway 唯一） → Task 1 (gen_trace_id) + Task 10 (gateway_setup_trace) + Task 11/12 (handler 接入)
- [x] §5.1 客户端 X-Trace-Id 透传 + 16 字节 hex 格式 → Task 10 resolve_trace_id 中 is_valid_trace_id 校验后透传，否则现生成
- [x] §5.1 透传链路 HTTP→Gateway→brpc metadata → P1 已落 metadata key（kMetaTraceId）+ Task 10 写入
- [x] §5.1 brpc → 所有后端 RPC → P1 已落 forward_auth_metadata 透传 4 字段
- [x] §5.1 → MQ headers → Task 6 / Task 8（rabbitmq.hpp 改造） / Task 13/14/15（三服务接入）
- [x] §5.1 → WS frame → Task 9 (proto 加 trace_id) + Task 15 (push 服务填充)
- [x] §5.2 spdlog 结构化 JSON 日志 → Task 3 (build_log_line) + Task 5 (改写 logger.hpp)
- [x] §5.2 自动写入 trace_id/user_id/device_id → Task 3 build_log_line 从 LogContext::current() 读
- [x] §5.2 LogContext (P1 已完成；本 plan 仅消费它)
- [x] §5.2 日志级别约定文档化 → Task 17 (log-levels.md)
- [x] §5.7 客户端重试策略约定文档化 → Task 19 (error-retry.md)
- [x] §5.8 监控告警接入文档化 → Task 18 (monitoring-conventions.md)
- [x] §5.2 日志格式 schema 文档化 → Task 16 (log-format.md)

**Spec 中本 plan 不覆盖（明确推到其他 plan / out-of-scope）**：

- §5.3 ResponseHeader 规范 / §5.4 ErrorCode 全集 / §5.5 ServiceError + HANDLE_RPC / §5.6 内部错误不泄漏 → 全部由 P1 完成
- §2.x JWT 鉴权 → P2
- §5.x ServiceError 客户端错误码映射代码 → 客户端 SDK 团队
- 接 OpenTelemetry / Jaeger → spec §0 明确不做

**Placeholder scan**：grep "TODO\|TBD\|实现后续\|similar to\|add error handling" 在本 plan 应为 0 命中。

**类型一致性**：
- `gen_trace_id() → std::string`（32 字符）；`is_valid_trace_id(const std::string&) → bool`
- `build_log_line(string_view level, string_view msg, FieldList) → std::string`；`FieldList = initializer_list<pair<string_view, string>>`，**值已 string 化**
- `mq_inject_trace_headers(map<string,string>&) → void`；`mq_extract_trace_id(const map<string,string>&) → string`
- `gateway_setup_trace(httplib::Request&, brpc::Controller&, string user_id="", string device_id="") → string trace_id`
- `MQClient::publish_confirm` 带 headers 重载 / 不带 headers 旧签名并存
- `MessageCallbackWithHeaders = function<ConsumeAction(const char*, size_t, bool, const map<string,string>&)>`

**向后兼容性**：

- LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG / LOG_TRACE / LOG_FATAL 宏名 + 参数列表（format, args...）保持不变；调用方代码零改动。**输出格式从文本改为 JSON**——本 plan 在 log-format.md 与 monitoring-conventions.md 已显式说明，下游 ELK / 告警如有按文本前缀（如 `[INFO]`）做 grep 的需切到 `level=info` 查询。
- spdlog 异步 + rotating file + flush 策略不变。
- MQ publish_confirm / consume 旧签名保留；新签名作为重载并存；非改造路径继续工作。
- `NotifyMessage.trace_id = 14` 是 optional 字段，对老客户端透明（不识别即忽略）。

**风险点与缓解**：

- 风险 1：spdlog 内部如果对 message 字符串再做 `{}` 占位符解析，"`{}`-in-payload" 会引发崩溃。缓解：`g_default_logger->log(level, "{}", _line);` 把整条 JSON 作为 `{}` 的单参数喂入，spdlog 不会二次解析 `_line` 内的字符。
- 风险 2：bthread 模式下 thread_local 语义。spec §1 / P1 已确认 brpc bthread 在不同 bthread 切换时 thread_local 各自独立（与 pthread 一致），LogContext 的 thread_local 安全。
- 风险 3：Gateway 改 34 个 handler 的机械脚本可能漏掉非标准模板的 handler。缓解：Task 12 Step 3 用 grep 计数对账；漏掉的按 Task 11 模板手工补齐。
- 风险 4：proto field 14 与未来字段冲突。缓解：14 是当前 NotifyMessage 顶层（非 oneof）下一个空 field number；保留 13 之后 oneof 内不会再增长（oneof 满 13 个）。

**未来工作**（不在本 plan 内）：

- W3C Trace Context propagation header `traceparent` 全套（含 span_id / trace_flags）：当前仅 trace_id 已与 16 字节 hex 兼容，扩展 span_id 等留待 OpenTelemetry 接入时一并做。
- 客户端 SDK 三端（iOS / Android / Web）落实 error-retry.md 的策略代码：客户端 plan。
- Identity 服务 Login / RefreshToken 业务接入 LogContext 的 user_id 字段（P2 起）：当前 user_id/device_id 空字符串占位，P2 完成后自动填充。
