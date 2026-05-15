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
| `7xxx` Device | `LIMIT_EXCEEDED` | 不重试;引导用户管理设备 |
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
