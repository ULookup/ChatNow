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
