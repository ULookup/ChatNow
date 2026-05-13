# ChatNow 架构现状与未来优化方向

> **版本**: v2.0 候选基线（含 `fix/v2.0-blockers-and-cleanup` 分支修复）
> **日期**: 2026-05-13
> **范围**: 重点是核心消息链路；外围（用户 / 好友 / 文件 / 语音）仅简述

---

## 一、当前架构（v2.0 候选基线）

### 1.1 服务拓扑

```
                 ┌──────────────┐
                 │   Client     │ HTTP / WebSocket
                 └─────┬─┬──────┘
                       │ │
                  HTTP │ │ WS (鉴权/心跳/ACK)
                       │ │
                ┌──────▼─┴──────┐
                │   Gateway     │  9000  无状态、纯 HTTP 入口
                └─┬─┬─┬─┬─┬─┬─┬─┘
                  │ │ │ │ │ │ │
       ┌──────────┘ │ │ │ │ │ └──────────┐
       │            │ │ │ │ │            │
       ▼            ▼ ▼ ▼ ▼ ▼            ▼
  ┌────────┐  ┌─────────────────┐    ┌────────┐
  │  User  │  │ Friend / Chat / │    │ Speech │
  │        │  │ File / ...      │    │        │
  └────────┘  └─────────────────┘    └────────┘
       ▲                                  ▲
       │                                  │
       │     ┌────────────────────────────┘
       │     │
   ┌───┴─────┴───┐    publish_confirm    ┌──────────────┐
   │  Transmite  │ ─────────────────────▶│  RabbitMQ    │
   │ (Ingest)    │   chat_msg_exchange   │ (FANOUT)     │
   └─────────────┘                       └──┬────────┬──┘
                                            │        │
                                  msg_queue_db    msg_queue_es
                                            │        │
                                            ▼        ▼
                                       ┌──────────────────┐
                                       │     Message      │
                                       │   (Store)        │
                                       │ ┌──────────────┐ │
                                       │ │ DB consumer  │ │
                                       │ │ ES consumer  │ │
                                       │ │ Outbox reaper│ │
                                       │ └──────────────┘ │
                                       └────┬───────┬─────┘
                            落库后 publish    │       │ 写 ES
                                msg_push_queue│       │
                                              ▼       ▼
                                       ┌─────────┐ ┌────┐
                                       │  Push   │ │ ES │
                                       │ (9001)  │ └────┘
                                       │ WS 终结  │
                                       │ 路由表   │
                                       │ ACK 重传 │
                                       └─────────┘
                                            │
                                      WS    │
                                            ▼
                                       ┌─────────┐
                                       │ Client  │
                                       └─────────┘
```

### 1.2 核心消息链路（端到端 7 步）

```
Step 1  Client          → POST /service/message_transmit/new_message
Step 2  Gateway         → MsgTransmitService.GetTransmitTarget (brpc)
Step 3  Transmite:
        │ a) SelectByClientMsg       幂等去重（client_msg_id 命中即返）
        │ b) RateLimiter.allow_*     用户级 + 会话级限流（固定窗口）
        │ c) Members.list / RPC 回填  群成员列表（缓存优先）
        │ d) SnowflakeId.Next()      worker_id 由 Redis 租约自动分配
        │ e) SeqGen.next_session_seq Redis INCR 会话内单调
        │ f) SeqGen.next_user_seq_batch (写扩散群批量；大群跳过)
        │ g) Publisher.publish_confirm  → chat_msg_exchange (FANOUT)
Step 4  RabbitMQ FANOUT  → msg_queue_db / msg_queue_es
Step 5  Message Service:
        │ DB consumer:
        │   a) 单事务: insert message + (写扩散群) user_timeline 批量
        │   b) 落库后 publish_confirm → msg_push_queue
        │   c) 投递失败 → enqueue 到 PushOutbox (Redis ZSET) → reaper 重投
        │   d) redelivered 二次失败 → NackDiscard 进 DLX
        │ ES consumer:
        │   仅 STRING 类型进索引；二次失败 NackDiscard
Step 6  Push Service:
        │ a) onPushMessage: per-uid 注入 user_seq + 序列化
        │ b) UnackedPush.push (uid, user_seq, ts) → ZSET 等 ACK
        │ c) 本机直推 / 跨实例 PushBatch (异步 brpc)
Step 7  Client → MSG_PUSH_ACK → push:
        │ UnackedPush.ack (zrem)
        │ 异步 UpdateAckSeq → message → DAO 原子 UPDATE GREATEST
```

### 1.3 已具备能力

| 维度 | 实现 |
|---|---|
| 幂等 | `client_msg_id` 唯一索引 |
| 单调 / 增量同步 | `(session_id, seq_id)` + `user_seq` 双游标 |
| 多实例发号 | Redis 租约 + atomic 自动分配 worker_id |
| 多实例推送 | `OnlineRoute` Redis SET<instance> + 跨实例 PushBatch |
| ACK 收敛 | UnackedPush ZSET + DAO 原子 GREATEST |
| 大群读扩散 | ≥200 切；timeline 仅写扩散群 |
| MQ DLX | redelivered 二次失败 NackDiscard 入死信 |
| 限流 | 用户级 + 会话级固定窗口 |
| 兜底 reaper | PushOutbox 单实例 Lua 租约 + 重投 |
| 防重号 | lease_lost watchdog 立即 abort |

### 1.4 当前架构的核心短板

| 短板 | 影响 | 修复优先级 |
|---|---|---|
| Publisher mandatory=false | broker 收到不等于 queue 收到，启动顺序错位会静默丢消息 | 高 |
| 跨实例 PushBatch 失败仅 LOG_WARN | 不入 outbox / unacked 不带 payload，重发链路不闭合 | 高 |
| 限流早于成员校验 | 攻击者伪造 uid 打爆受害人计数器 | 高 |
| Members 缓存 read-then-compute | 与成员变更存在写扩散漏人窗口 | 中 |
| 大群读/写扩散切换 | GetRecentMsg 未 union 两条路径，跨阈值会丢消息 | 中 |
| fallback worker_id | Redis 不通时与正常 slot 撞号 | 中 |
| 0 监控 | brpc bvar 未暴露；无 Prom / Grafana / 日志聚合 | 高 |
| 无 healthcheck | 容器层依赖 restart: always | 中 |
| 文件类消息走"客户端前置上传" | 失败后无 GC，孤儿文件累积 | 低 |
| 限流是固定窗口 | 边界放行 ≈2N，对反爬不严格 | 低 |

---

## 二、未来架构优化方向

按"投入产出比 × 上线必要性"排序，分三档。

### 2.1 第一优先级 — 上线候选必须补齐

#### A. Publisher 一致性升级（B2.1）
**问题**：`publish_confirm` 在 `mandatory=false` 下，broker 接收 ≠ 至少有一个 queue 接收。Transmite 先于 message 起，exchange 已声明但无 binding，FANOUT 直接 drop。

**方向**：
- `MQClient::publish_confirm` 暴露 `mandatory=true` 选项，监听 `basic.return`
- Returned 视作 `PublishStatus::Lost`，Transmite 端走业务侧重试 / 入 outbox
- queue 改为 `durable=true`，由运维或 message 服务声明，broker 重启后保留

#### B. 推送链路完整闭环（M5/M6 相邻）
**问题**：跨实例 PushBatch 失败只 LOG_WARN；UnackedPush 只存 user_seq，重发时还要回查 message 表。

**方向**：
- UnackedPush 存 `(user_seq, message_id)` 二元组（或直接 payload 摘要），心跳重发不必再 RPC
- 跨实例 PushBatch 失败 → 入 outbox 或本端 unacked 提前抢救
- 长尾路径：每个 uid 一条 dead-letter Sorted Set，超过 N 次重发的 user_seq 进入「需要客户端主动 pull」状态

#### C. 安全闸门
**问题**：限流键直接用请求里的 `uid`，攻击者可以把任意 uid 打爆。

**方向**：
- 限流前先做 Members 校验（已经查过，只是顺序问题）
- 限流键改为 (request_uid, real_session_uid) 复合，攻击者打不到第三方
- 配套：内容审核接入（敏感词 / 反垃圾），先做被动埋点 → 后做主动拦截

#### D. 监控与可观测性
**问题**：v2.0 是 0 监控，链路出问题靠 grep 日志。

**方向（按工作量分层）**：

| 层级 | 内容 | 工时 |
|---|---|---|
| L1 | brpc 内置 `/status /vars /rpcz /health` 暴露 | 1 天 |
| L2 | Prometheus + Grafana + 中间件 exporter (rabbitmq, redis, mysql, es) | 3-5 天 |
| L3 | 业务 bvar：`message_publish_total / unacked_size / push_outbox_lag / lease_lost_total / dlx_total / rate_limited_total` | 1 周 |
| L4 | OpenTelemetry C++ + Jaeger 全链路 | 1-2 周 |
| L5 | promtail/Filebeat → Loki/ES 日志聚合 | 2-3 天 |

**最低生产监控套件 5 条告警**：DLX 增量异常 / outbox 积压 > 1000 / lease_lost > 0 / ACK 收敛 P99 > 30s / 在线连接突降 50%。

---

### 2.2 第二优先级 — 容量与一致性

#### E. Members 缓存竞态修复（M8）
**方向**：
- chatsession 在每次成员变更时 bump session 的 `version`，缓存项写入时也带 version
- transmite 读缓存后比对 version，不一致则回退到 chatsession RPC
- 或者改为「在 chatsession 写完后主动 push 一条 `MemberDelta` 到 Redis Stream」，transmite 端订阅同步

#### F. 大群读/写扩散迁移期（M9）
**方向**：
- `GetRecentMsg` 永远 union 两条路径（timeline + recent_by_seq）取并集 + dedup
- 阈值变化时打迁移 flag，老群保留写扩散直到下一个 epoch
- 或者：把"读扩散群仅写主表"放宽为"大群也写 user_timeline 但是延迟批写"，避免历史不连续

#### G. 文件链路重构
**问题**：现在客户端前置上传 file_id 然后才能发消息；上传失败 / 用户取消会留孤儿文件。

**方向**：
- 引入「pending_file」状态：上传完成但消息未落库的 file_id，TTL 过期 GC
- 消息落库时把 file_id 状态翻成 confirmed
- 配套异步 GC worker（同 Outbox reaper 模式：单实例 Lua 租约）

#### H. 限流升级
**方向**：
- 固定窗口换 redis-cell 的 `CL.THROTTLE`（GCRA 算法），消除窗口边界放行
- 两层：边缘 nginx / sidecar 粗粒度（连接 / 请求 QPS）+ 业务 RateLimiter 细粒度（用户 / 会话）
- 接入风控信号：异常 IP / 高频小号 / 黑名单进单独白名单

#### I. 一致性增强 — Outbox 模式正式化
**方向**：
- DB 落库与 push_queue 投递改为「本地事务 + outbox 表」模式（而不是 publish_confirm 失败再 enqueue Redis）
- outbox 表与 message 表同事务写入，reaper 扫该表轮询投递
- 比 Redis ZSET 兜底强一致：DB 事务保证「消息存在 ↔ outbox 行存在」

---

### 2.3 第三优先级 — 长期演进

#### J. 服务拆分进一步收敛
- `Transmite` 与 `Message` 之间的 InternalMessage proto 太胖（携带完整成员列表），随群规模线性增长 → 改为"只携带 session_id + 必要元数据，message 自己拉成员"或 push 端拉
- 把"成员列表 / 路由表 / Outbox"等 Redis 数据从单库剥离为 Redis Cluster 分片
- 引入消息聚合服务（Mention / Reaction / Read receipt 各自独立，避免主表膨胀）

#### K. 推送服务多机房 / 多活
- OnlineRoute 现在是单 Redis 实例；多机房需要：
  - 跨机房 Redis 路由表分区（用户归属机房 → 主路由 → fallback）
  - PushBatch 跨机房走专线，本机房优先
- 配套：DNS 智能解析 / Anycast 入口

#### L. 消息排序保证
**问题**：当前同一会话不同 client 来源消息的顺序由 SeqGen INCR 决定，但 publish 失败重试 / DLX 重投会让 seq 出现"留洞"。

**方向**：
- 引入 sequencer 服务：消息按 session_id 路由到固定 sequencer 分片，单线程顺序授号
- session_seq 严格连续，客户端可检测 hole 主动拉补
- 或者：客户端容忍 hole，定期主动 GetRecentMsg 自检

#### M. 端到端加密 / 隐私
- 现在 message 主表存明文，ES 也是明文索引
- 长期方向：客户端 E2EE（Signal Double Ratchet 协议），服务端只存密文
- ES 改为 fragment-based 同态搜索，或放弃服务端搜索改客户端本地索引
- 是否要做取决于产品定位（toC IM 必须，toB 协作可选）

#### N. 容器化 / K8s 落地
- 8 个服务都有 dockerfile，但单机 compose
- 方向：
  - 每个服务 Helm chart，HPA 按 brpc QPS 自动扩
  - PVC 替代宿主机卷挂载（mysql / redis / es 走 StatefulSet）
  - secret 迁到 Vault 或 K8s Secret，禁止 yml 硬编码
  - Service Mesh（Istio）接管 mTLS / 限流 / 灰度
  - 健康检查 / 优雅停机 / PDB

#### O. CI/CD 与质量门
- 当前是手工编译 + push
- 方向：
  - GitHub Actions：每 PR 跑 cmake build + cppcheck / clang-tidy + 单元测试
  - merge to main 触发镜像构建 + 推送到 registry
  - 部署阶段：金丝雀（先 5% 流量）→ 全量；CI 与监控的 SLO 联动自动回滚
  - E2E 测试：起 docker-compose 全栈，跑端到端消息流程

---

## 三、立刻可做 vs 渐进做（优先级矩阵）

```
                  立刻做                    可灰度做
                ┌───────────────────┐  ┌───────────────────┐
   高影响       │ A Publisher 一致性 │  │ E Members 竞态    │
                │ B 推送链路闭环      │  │ F 大群迁移期       │
                │ C 限流安全          │  │ I Outbox 模式      │
                │ D L1/L3 监控        │  │ K 多活             │
                └───────────────────┘  └───────────────────┘
                ┌───────────────────┐  ┌───────────────────┐
   低影响       │ N healthcheck      │  │ G 文件链路        │
                │ O CI 构建          │  │ H 限流升级         │
                │                    │  │ J 服务再拆分       │
                │                    │  │ L sequencer       │
                │                    │  │ M E2EE            │
                └───────────────────┘  └───────────────────┘
```

**建议路线**：

- v2.1（2 周）：A + B + C + D 的 L1/L3 + N + O — 让上线候选真正进生产
- v2.2（1 月）：E + F + I + L4 OpenTelemetry — 容量与一致性
- v2.3 之后（季度级）：G/H/J/K/L/M — 长期演进

---

## 四、一句话总结

**当前架构**：v2.0 已经从"单 Gateway 直推 + 单事务写扩散"演进到"Ingest / Store / Push 三角架构 + 增量同步双游标 + 高可用兜底"，本 PR 修完后核心链路通畅、ACK 闭环、多实例可路由、关停安全。

**下一步重点**：把"publisher → broker → consumer → push → client"这条链上的每一段都从"best-effort"变成"可观测 + 可恢复 + 可重试 + 可审计"。第一优先级是 Publisher mandatory + 推送闭环 + 监控 — 没这三样，再多业务 feature 都是在沙地盖楼。
