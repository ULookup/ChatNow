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
