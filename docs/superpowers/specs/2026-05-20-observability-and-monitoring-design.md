# ChatNow 可观测性 & 生产监控体系设计

## 背景与目标

ChatNow 3.0 已落地 P1（结构化日志）、P8（trace_id 全链路），但目前 **没有任何 metrics 暴露端点**，缺乏：

- 服务级 QPS / 延迟 / 错误率指标
- 中心化日志查询（当前 ssh + grep 查各节点文件）
- 分布式追踪可视化
- 自动化告警

**目标**：上线前补齐 metrics、日志聚合、追踪可视化、告警四大支柱，使团队可以在 Grafana 统一面板中从告警 → 指标 → 日志 → 追踪一键下钻排障。

**约束**：
- K8s 部署，中等规模（1-10 万 DAU）
- C++17 + brpc 框架，高性能要求（metrics 写入不能成为瓶颈）
- 团队愿意学习 Prometheus/Grafana 栈

---

## 方案概览

选择 **Thin SDK + Sidecar** 模式（而非全量 OpenTelemetry SDK 或纯 brpc 内置方案）：

- **Metrics**：brpc bvar（thread-local 无锁）→ Prometheus 格式暴露 → Prometheus Operator 采集
- **Logs**：spdlog 双 sink（stdout + file）→ Promtail → Loki
- **Traces**：OTel C++ SDK 轻量封装 → Jaeger Agent Sidecar → Jaeger
- **可视化**：Grafana 统一面板，Data Link 串联三条腿

---

## 架构

```
                          ┌─────────────────────────────────┐
                          │            Grafana              │
                          │  仪表板 · Explore · Trace View   │
                          └──────┬──────┬──────┬────────────┘
                                 │      │      │
                    ┌────────────┘      │      └────────────┐
                    ▼                   ▼                   ▼
              Prometheus              Loki               Jaeger
              (指标存储+告警)        (日志聚合)          (追踪存储)
                    ▲                   ▲                   ▲
                    │                   │                   │
   ┌────────────────┼───────────────────┼───────────────────┼────────────┐
   │  k8s           │                   │                   │            │
   │  ┌─────────────┴─────┐  ┌─────────┴──────┐  ┌────────┴─────────┐  │
   │  │ ServiceMonitor    │  │ Promtail       │  │ Jaeger Agent     │  │
   │  │ (声明式采集)       │  │ (DaemonSet)    │  │ (Sidecar 注入)    │  │
   │  └────────┬──────────┘  └────────┬───────┘  └────────┬─────────┘  │
   │           │                      │                    │            │
   │  ┌────────┴──────────────────────┴────────────────────┴─────────┐  │
   │  │ ChatNow Pod                                                   │  │
   │  │  ┌──────────────────────────────────────────────────────────┐ │  │
   │  │  │ chatnow-gateway (brpc :9000)                              │ │  │
   │  │  │  · bvar → /brpc_metrics (Prometheus 格式)                 │ │  │
   │  │  │  · OTel SDK → span creation (HANDLE_RPC 宏中注入)         │ │  │
   │  │  │  · spdlog → stdout (JSON 行) + file (兜底)                 │ │  │
   │  │  └──────────────────────────────────────────────────────────┘ │  │
   │  └──────────────────────────────────────────────────────────────┘  │
   │  ...每个服务 Pod 结构相同                                          │
   └────────────────────────────────────────────────────────────────────┘
```

### 数据流

```
Metrics: ChatNow bvar → /brpc_metrics → Prometheus scrape (30s) → Grafana panel + AlertManager
Logs:    ChatNow JSON stdout → Promtail tail → Loki → Grafana Explore (按 trace_id 搜索)
Traces:  OTel SDK span → Jaeger Agent (UDP) → Jaeger Collector → Jaeger Query → Grafana Trace View
告警:    Prometheus rule 触发 → AlertManager 分组/去重/抑制 → Slack/钉钉 Webhook
```

---

## Metrics 指标体系

### 1. brpc 内置指标（零代码）

```cpp
brpc::ServerOptions options;
options.has_builtin_services = true;        // /status /vars /connections
options.enable_prometheus_exporter = true;  // /brpc_metrics (Prometheus 格式)
```

自动获得：`process_cpu_usage`、`process_memory_*`、`rpc_server_*_count`、`rpc_server_*_latency_*`、`connection_count`、`bthread_*` 等 40+ 指标。

### 2. 公共横切指标（HANDLE_RPC 宏集中注入）

在 `common/infra/metrics.hpp` 中定义，`common/error/handle_rpc.hpp` 宏中引用：

| bvar 变量 | 类型 | Prometheus 名称 | 说明 |
|---|---|---|---|
| `g_rpc_total` | Adder&lt;int64&gt; | `chatnow_rpc_requests_total` | 按 service/method label |
| `g_rpc_error_total` | Adder&lt;int64&gt; | `chatnow_rpc_errors_total` | 按 error_code label |
| `g_rpc_latency` | LatencyRecorder | `chatnow_rpc_latency_ms` | P50/P95/P99 histogram |
| `g_auth_missing_total` | Adder&lt;int64&gt; | `chatnow_auth_missing_total` | metadata 缺失计数 |

### 3. 各服务业务指标

**Gateway**：`http_requests_total`、`jwt_verify_errors_total`、`backend_timeouts_total`、`backend_unavailable_total`

**Message**：`mq_consume_total`、`mq_consume_lag_ms`（histogram）、`db_msg_insert_errors_total`、`es_index_errors_total`、`outbox_size`（gauge）

**Push**：`ws_connections`（gauge）、`ws_messages_sent_total`、`ws_messages_failed_total`、`cross_instance_fanout_total`、`push_to_user_latency_ms`（histogram）

**Transmite**：`mq_publish_total`、`mq_publish_latency_ms`、`mq_publish_failed_total`

**Media**：`upload_apply_total`、`s3_presign_latency_ms`、`quota_exceeded_total`

**Identity**：`login_total`、`jwt_sign_latency_ms`

**Conversation**：`members_cache_hit_total` / `members_cache_miss_total`（接已有 LocalCache）

### 4. 命名规范

```
chatnow_{domain}_{metric}_{unit}
```

Label 维度：`service`（服务名）、`method`（RPC 方法）、`error_code`（错误码）、`queue`（MQ 队列名）、`instance`（Pod 名）。

### 5. 性能影响

bvar 使用 thread-local 无锁累加器，写入开销约 1-2ns。一次 RPC handler 中增加 10 个 bvar 埋点总开销约 20ns，而一次 Redis 查询约 0.5ms。相对开销为 **0.001% 级别**，不影响高并发目标。

---

## 日志聚合

### 改动

`common/infra/logger.hpp` 中 `init_logger()` 增加一个 `stdout_color_mt` sink，spdlog 同时写 stdout + 文件。JSON 格式不变。

### 采集

- **方案 A（推荐）**：k8s 自动采集容器 stdout，Promtail DaemonSet tail 节点日志目录
- **方案 B（备选）**：Promtail sidecar tail 挂载的日志文件（`/var/log/chatnow/*.log`）

### 查询

Grafana Explore → Loki datasource：

```logql
{service="message", level="error"} |= "trace_id_value"
{app="chatnow-gateway"} | json | line_format "{{.msg}}"
```

---

## 分布式追踪

### 实现方式

在 `HANDLE_RPC` 宏中一次性注入 span 创建（不侵入各 handler）：

1. 入口：从 brpc attachment 或 HTTP header 提取 trace context（复用已有 `extract_auth` + `forward_auth` 机制）
2. 创建 child span，设置 `trace_id`、`user_id`、`service`、`method` 等属性
3. body 执行成功 → `span->SetStatus(Ok)`；抛异常 → `span->SetStatus(Error)`
4. 出口：LogContextGuard 析构前 `span->End()`

### MQ 链路传递

- 发布端：span context 序列化到 AMQP header（扩展已有 `mq/trace_headers.hpp`）
- 消费端：从 AMQP header 恢复 span context，创建 child span

### 组件

- Jaeger Operator 部署 collector + query
- Jaeger Agent 以 sidecar 注入每个 ChatNow Pod
- OTel C++ SDK（仅 tracing 部分）编译进 ChatNow 服务

---

## 告警规则

### 告警分级

- **critical**：影响用户功能，需立即处理 → Slack #oncall + 钉钉
- **warning**：需关注，可能恶化 → Slack #chatnow-alerts
- **info**：仅记录

### 规则列表

| 分类 | 规则 | 条件 | 级别 |
|---|---|---|---|
| 服务 | ServiceDown | `up == 0` for 1m | critical |
| 服务 | HighErrorRate | 错误率 >5% for 5m | critical |
| 服务 | HighLatencyP99 | P99 >3s for 5m | warning |
| 服务 | HighLatencyP95 | P95 >1s for 5m | warning |
| 服务 | AuthMissingSpike | metadata 缺失 >10/s for 5m | critical |
| MQ | MQConsumeLagHigh | P50 lag >30s for 5m | critical |
| MQ | OutboxGrowing | outbox size >1000 for 10m | warning |
| MQ | MQPublishFailing | 发布失败 >0 for 5m | critical |
| Push | WSConnectionDrop | 连接数 <阈值 for 5m | critical |
| Push | WSMessageFailRate | 下发失败率 >10% for 5m | warning |
| 基础设施 | RedisNodeDown | redis_exporter | critical |
| 基础设施 | MySQLTooManyConnections | 连接数 >80% for 5m | critical |
| 基础设施 | MySQLSlowQueries | >10/min for 5m | warning |
| 基础设施 | ESClusterRed/Yellow | 非 green | critical |
| 基础设施 | ContainerOOM/HighCPU | cadvisor | critical |
| 基础设施 | MinIODiskFull | >85% | warning |

---

## 部署配置

### 监控栈（Helm Charts）

| 组件 | Chart | 用途 |
|---|---|---|
| Prometheus + AlertManager + Grafana | `kube-prometheus-stack` | 指标采集存储告警面板（一条龙） |
| Loki + Promtail | `loki-stack` | 日志聚合 |
| Jaeger | `jaeger-operator` | 追踪收集 |

### 基础设施 Exporters（Helm Charts）

- `prometheus-community/prometheus-mysql-exporter`
- `prometheus-community/prometheus-redis-exporter`
- `prometheus-community/prometheus-elasticsearch-exporter`
- `prometheus-community/prometheus-rabbitmq-exporter`
- MinIO 自带 Prometheus endpoint

### ServiceMonitor

每个 ChatNow 服务一个 ServiceMonitor CRD，声明采集 `/brpc_metrics` 每 30s。

### ChatNow 代码侧改动清单

| 文件 | 改动类型 | 内容 |
|---|---|---|
| `common/infra/metrics.hpp` | **新增** | 公共 bvar 声明 + label 注册 |
| `common/infra/otel_trace.hpp` | **新增** | OTel Tracer 初始化 + span helper |
| `common/error/handle_rpc.hpp` | **修改** | 宏中新增 metrics 计数 + span 创建 |
| `common/infra/logger.hpp` | **修改** | init_logger 加 stdout sink |
| `deploy/k8s/base/*.yaml` | **新增** | 各服务 Deployment + Service + ServiceMonitor |
| `deploy/k8s/overlays/prod/*.yaml` | **新增** | 生产 overlays |
| `deploy/monitoring/` | **新增** | Helm values + alert rules + dashboard JSON |

### K8s 目录结构

```
deploy/
├── k8s/
│   ├── base/
│   │   ├── namespace.yaml
│   │   ├── gateway-deployment.yaml
│   │   ├── gateway-service.yaml
│   │   ├── gateway-servicemonitor.yaml
│   │   ├── message-deployment.yaml
│   │   ├── message-service.yaml
│   │   ├── message-servicemonitor.yaml
│   │   └── ...（其他服务同理）
│   └── overlays/
│       └── prod/
│           ├── kustomization.yaml
│           └── patches/
│               ├── replicas.yaml
│               └── resources.yaml
└── monitoring/
    ├── prometheus-values.yaml
    ├── loki-values.yaml
    ├── alert-rules.yaml
    └── dashboards/
        ├── chatnow-overview.json
        ├── chatnow-message.json
        └── chatnow-push.json
```

---

## 排障流程示例

```
1. Grafana Dashboard 看到 Gateway P99 延迟飙升
2. 点击异常时间点 → 跳到 Loki 日志，按 trace_id 过滤
3. 点击 trace_id → 跳到 Jaeger Trace View，定位耗时 Spans
4. 发现 Message.onDBMessage 耗时最长 → 跳回 Loki 查该服务日志
5. 确认 DB 慢查询 → 跳转 MySQL dashboard 定位具体 SQL
```

Grafana Data Link 配置实现一键跳转，零代码。

---

## License

仅作学习与项目演示用途。
