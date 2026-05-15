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
