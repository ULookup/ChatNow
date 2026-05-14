# 核心消息链路分析：与主流 IM 对比及现存缺陷

> 分析日期：2026-05-13  
> 范围：客户端发送 → 服务端处理 → 消息落库 → 推送下发的完整链路  
> 对比基准：微信、飞书、钉钉、Discord

---

## 1. 完整消息链路（源码级追踪）

### 1.1 发送路径：Client → Gateway → Transmite → MQ → Message → Push → Client

```
Client                  Gateway              Transmite              RabbitMQ           Message              Push                Client(Receiver)
  │                        │                     │                     │                  │                   │                      │
  │ POST /new_message      │                     │                     │                  │                   │                      │
  │───────────────────────>│                     │                     │                  │                   │                      │
  │                        │ brpc:GetTransmit   │                     │                  │                   │                      │
  │                        │ Target              │                     │                  │                   │                      │
  │                        │────────────────────>│                     │                  │                   │                      │
  │                        │                     │                     │                  │                   │                      │
  │                        │                     │─ Step 0: 文件校验     │                  │                   │                      │
  │                        │                     │─ Step 1: 幂等去重 ──>│SelectByClientMsg │                   │                      │
  │                        │                     │                     │                  │                   │                      │
  │                        │                     │─ Step 1.5: 限流       │                  │                   │                      │
  │                        │                     │─ Step 2: 用户+成员RPC  │                  │                   │                      │
  │                        │                     │─ Step 3: Redis INCR   │                  │                   │                      │
  │                        │                     │  session_seq+user_seq │                  │                   │                      │
  │                        │                     │─ Step 4: Snowflake ID  │                  │                   │                      │
  │                        │                     │─ Step 6: publish_conf  │                  │                   │                      │
  │                        │                     │─────────────────────>│                  │                   │                      │
  │                        │                     │                     │ FANOUT           │                   │                      │
  │    <── brpc response   │<────────────────────│                     │────────┐         │                   │                      │
  │<── HTTP response       │                     │                     │        │         │                   │                      │
  │                        │                     │                     │   db_queue         │                   │                      │
  │                        │                     │                     │──>|onDBMessage     │                   │                      │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │  ┌─ 事务: message + timeline    │                      │
  │                        │                     │                     │        │  │  (大群跳过 timeline)          │                      │
  │                        │                     │                     │        │  └─ publish_confirm push_queue─│──> push_queue        │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │   es_queue         │                   │                      │
  │                        │                     │                     │──>|onESMessage     │                   │                      │
  │                        │                     │                     │        │  (仅 STRING 类型)              │                      │
  │                        │                     │                     │        │  appendData → ES              │                      │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │    push_queue     │                      │
  │                        │                     │                     │        │           │───────────────────> onPushMessage        │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │ Phase 1: _local_send  │
  │                        │                     │                     │        │           │                   │ (per-conn mutex WS)   │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │ Phase 2: PushBatch    │
  │                        │                     │                     │        │           │                   │──> peer Push instance │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │ UnackedPush ZSET     │
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │───────── WS ────────>│
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │<─── MSG_PUSH_ACK ────│
  │                        │                     │                     │        │           │                   │                      │
  │                        │                     │                     │        │           │                   │ zrem unacked         │
  │                        │                     │                     │        │           │                   │ + UpdateAckSeq RPC   │
```

### 1.2 链路各环节详解

| # | 环节 | 文件:行号 | 关键操作 | 故障模式 |
|---|------|-----------|----------|----------|
| 0 | 入参校验 | `transmite_server.h:73-94` | 文件类型必须有 file_id | 客户端未前置上传 → 拒绝 |
| 1 | 幂等去重 | `transmite_server.h:98-120` | brpc 同步调用 `SelectByClientMsg` | message 服务不可用 → 跳过（可能产生重复消息） |
| 1.5 | 限流 | `transmite_server.h:127-138` | 固定窗口 INCR+EXPIRE | Redis 不可用 → 默认放行 |
| 2 | 成员列表 | `transmite_server.h:148-188` | Redis SET 缓存优先，RPC 兜底 | DEL+warm 存在竞争窗口 |
| 3 | Seq 分配 | `transmite_server.h:203-207` | `next_session_seq` + `next_user_seq_batch` | Redis 重启 → INCR 归零 → 主键冲突 |
| 4 | 消息组装 | `transmite_server.h:210-241` | Snowflake ID + seq + user_seqs | 大群跳过 user_seq 分配 |
| 6 | MQ 投递 | `transmite_server.h:253-285` | `publish_confirm` + 异步 done | MQ 不可用 → 返回失败，客户端重试 |
| 7 | DB 消费 | `message_server.h:575-701` | 事务写入 message + timeline | 唯一索引冲突 → 幂等丢弃；首次失败 → requeue |
| 8 | ES 消费 | `message_server.h:762-805` | 仅 STRING 类型写 ES | ES 不可用 → requeue → DLX（无 outbox 兜底） |
| 9 | Push 投递 | `message_server.h:667-685` | DB 成功→publish_confirm push_queue | 失败 → PushOutbox ZSET + reaper 重试 |
| 10 | Push 消费 | `push_server.h:143-231` | _local_send + 跨实例 PushBatch | 跨实例失败 → LOG_WARN 后消息丢失 |
| 11 | 本地发送 | `push_server.h:355-371` | per-conn mutex + WS binary send | conn 关闭 → 跳过，依赖 unacked 重传 |
| 12 | ACK 处理 | `push_server.h:234-269` | zrem unacked + async UpdateAckSeq | UpdateAckSeq 失败 → 下次 ACK 自然追赶 |
| 13 | 心跳重发 | `push_server.h:282-348` | peek_due → GetOfflineMsg → resend | message 服务不可用 → 无法重发 |

---

## 2. 与主流 IM 的逐项对比

### 2.1 消息摄入（Ingestion）

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 入口协议 | HTTP POST → brpc RPC | 私有长连接协议 | HTTP/2 + gRPC | 私有长连接 | WebSocket + HTTP REST |
| 幂等机制 | client_msg_id 唯一索引 | 客户端 seq + 服务端去重表 | idempotency-key header | 客户端 seq | nonce (snowflake) |
| 限流 | 固定窗口 INCR（有边界缺陷） | 令牌桶（内核级） | 多层令牌桶 | 令牌桶 + 自适应 | 全局 rate limit |
| 成员查询 | Redis SET 缓存优先 | 内存缓存 + 版本号 CAS | singleflight + Redis | 本地缓存 + Redis | 本地缓存（guild 状态） |

**核心差异：**
- 微信使用**版本号 CAS** 保证缓存不会回退，ChatNow 的 DEL+warm 存在回退窗口
- 飞书使用 **singleflight** 防缓存击穿，ChatNow 无此机制
- 钉钉的限流是**多层级联**（用户→会话→全局），ChatNow 只有用户+会话两层且不关联

### 2.2 消息 ID / Seq 策略

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 全局 ID | Snowflake (41+10+12) | 自研 ID 生成器 | Snowflake 变体 | Snowflake | Snowflake |
| 会话 seq | Redis INCR (im:seq:ssid) | DB AUTO_INCREMENT | Redis INCR + DB 回填 | Redis INCR | 无（时间排序） |
| 用户 seq | Redis INCR (im:seq:uid) | 服务端自增序列 | 自增序列 + 持久化 | 自增序列 | 客户端序号 |
| Redis 故障恢复 | **无**（backfill 定义了但从未调用） | 持久化 seq 表 | 启动时从 DB 回填 | DB 双写 seq 表 | 不适用 |
| 序号目的 | 双游标：session_seq（会话有序）+ user_seq（增量同步） | 用户级 seq 用于收件箱同步 | 用户级 seq | 会话级 seq | — |

**核心差异：**
- ChatNow 的双游标设计（session_seq + user_seq）本身是正确的，这是 IM 的成熟模式
- **致命缺陷**：`SeqGen::backfill_session/user()` 在 `data_redis.hpp:236-254` 定义了，但全代码库没有任何调用。Redis 重启后 INCR 从 1 开始，与 DB 中已有 seq 冲突
- 微信的 seq 生成器数据持久化到本地文件，重启后回读
- 飞书的启动回填是自动化流程的一部分，ChatNow 的缺失

### 2.3 消息存储（写扩散 vs 读扩散）

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 小群（≤200） | 写扩散：每成员一条 timeline | 写扩散 | 写扩散 | 写扩散 | — |
| 大群（>200） | 读扩散：仅写 message 主表 | 写扩散（万人群特殊优化） | 读扩散 | 读扩散 | 全读扩散 |
| 阈值 | 硬编码 200 | 动态（按活跃度） | 可配置 | 可配置 | — |
| 存储引擎 | MySQL (ODB ORM) | 自研 KV 存储 | TiDB | MySQL 分库分表 | Cassandra/ScyllaDB |
| 事务保证 | message + timeline 在同一 MySQL 事务 | WAL 先行 + 异步写 | 分布式事务 | 本地事务 | 最终一致 |

**核心差异：**
- ChatNow 的 200 人阈值是**硬编码常量** `transmite_server.h:26`，微信是动态调整的
- 钉钉的大群路径还有**定时批量刷新**读扩散缓存，ChatNow 没有
- Discord 全读扩散是因为消息量极大，写扩散成本不可接受

### 2.4 ES 索引（搜索）

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 写入方式 | FANOUT MQ 并行消费（DB 与 ES 独立） | 异步流水线，写入成功后才索引 | 异步，DB 成功后发事件 | **CDC binlog → ES** | 异步批量 |
| 一致性保证 | **无**（DB 失败 ES 可能成功） | 写入成功 → 索引事件 | 事务提交 → 索引事件 | binlog 保证先后顺序 | 最终一致 |
| 失败处理 | NackRequeue → DLX（无 outbox） | 重试队列 + 死信 | 重试 + 降级 | binlog 回溯 | 丢弃（允许搜索不到） |
| 索引内容 | 仅 STRING 类型消息 | 文本 + 可搜索附件 | 全类型（含富文本解析） | 全类型 | 文本（可搜索） |
| Outbox | 无（已在设计改造中） | 有 | 有 | 天然支持（CDC） | 无（允许丢） |

**核心差异：**
- 钉钉的 **CDC 方案**是最优解：ES 内容天然是 DB 的子集，不会有幽灵数据
- ChatNow 的 FANOUT 方案中 DB 和 ES 是**对等消费者**，导致 ES 可能有 DB 没有的数据（ghost search results）
- 已在 `docs/superpowers/specs/2026-05-13-es-dual-write-redesign.md` 中设计了改造方案

### 2.5 推送链路（Push Delivery）

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 连接协议 | WebSocket (port 9001) | 私有长连接 | WebSocket | 私有长连接 | WebSocket |
| 在线路由 | Redis SET (im:online:uid) | 路由表（内存） | Redis + 本地缓存 | 路由服务 | Guild sharding |
| 多实例推送 | PushBatch brpc 跨实例转发 | 内部 RPC | gRPC | 内部 RPC | Guild 路由 |
| 跨实例失败处理 | **LOG_WARN 后丢弃** | 重试 + 离线兜底 | 重试 + 降级 | 重试队列 | 不适用 |
| ACK 机制 | UnackedPush ZSET + heartbeat 重传 | 超时重传 + 拉取 | 接收确认 + 重推 | 接收确认 | 无（拉模式） |
| 离线消息 | GetOfflineMsg (user_seq 增量) | 收件箱拉取 | 增量同步 | 增量拉取 | 拉取通道历史 |

**核心差异：**
- ChatNow 的跨实例 PushBatch 失败只有 `LOG_WARN`（`push_server.h:225`），消息**静默丢失**，没有 outbox 或重试机制
- 微信的推送有**完整的确认-重传-离线降级**链路
- Discord 采用拉模式（客户端主动拉取），避免了推送丢失问题
- ChatNow 的心跳重传依赖 Message 服务 RPC（`_on_heartbeat_resend`），如果 Message 服务不可用，无法补送

### 2.6 缓存策略

| 维度 | ChatNow | 微信 | 飞书 | 钉钉 | Discord |
|------|---------|------|------|------|---------|
| 成员缓存 | Redis SET + 全量 DEL 失效 | 版本号 CAS | singleflight + TTL | 版本号 + TTL | 本地内存 |
| 用户信息缓存 | **无**（每次 RPC） | 多级缓存 | Redis + 本地 | 多级缓存 | 本地内存 |
| 缓存防击穿 | **无** | 互斥锁 | singleflight | singleflight | 不适用 |
| 缓存防雪崩 | 固定 TTL（同时过期） | 随机 TTL 偏移 | 随机 TTL | 随机 TTL | 不适用 |

已在 `docs/superpowers/specs/2026-05-13-cache-strategy-redesign.md` 中详细设计了缓存改造方案。

---

## 3. 现存缺陷清单

### 3.1 BLOCKER 级（会导致数据丢失或服务不可用）

#### B1. SeqGen Redis 重启后无回填——消息静默丢失

- **位置**：`data_redis.hpp:236-254` 定义了 `backfill_session/user()`，但全代码库无调用
- **触发条件**：Redis 重启、failover、key 被驱逐
- **后果**：`next_session_seq()` 返回 1，但 DB 中 `uk_session_seq` 已有值 >1。INSERT 撞唯一约束 → 事务回滚 → 消息丢失。客户端已收到 `GetTransmitTargetRsp`（seq_id 已返回），但消息未落库。
- **影响范围**：所有会话。Redis 重启后第一条消息必然触发。
- **对比**：飞书启动时自动从 DB `SELECT MAX(seq_id)` 回填；微信持久化到本地文件。

#### B2. Members 缓存 DEL + warm 竞争——成员列表过时

- **位置**：`chatsession_server.h:225,441,488,890` 调用 `invalidate()`（即 DEL），`transmite_server.h:188` 调用 `warm()`（即 SADD）
- **触发条件**：成员变更与消息发送并发
- **后果**：DEL 后，一个旧 RPC 返回的数据先到达 warm，把已删除的成员重新加回 SET。新消息推送给已退群用户（安全风险），或漏掉新成员。
- **对比**：微信使用版本号 CAS 保证不回退；飞书使用增量 SADD/SREM 而非全量替换。

#### B3. 跨实例 PushBatch 失败——消息不送达且无兜底

- **位置**：`push_server.h:204-207`（channel 不可达 → LOG_WARN → 跳过），`push_server.h:224-226`（RPC 失败 → LOG_WARN）
- **触发条件**：目标 Push 实例不可达（崩溃、网络分区、服务发现延迟）
- **后果**：用户在另一台 Push 实例上完全收不到消息。DB 已落库，但推送静默丢失。无 outbox、无重试、无死信队列。
- **对比**：DB→push_queue 路径有 PushOutbox 兜底；但 push_queue→跨实例推送无兜底。这是不对称的保护。

#### B4. ES 与 DB 独立消费——幽灵搜索结果

- **位置**：`message_server.h:575-806`（两个独立 consumer）
- **触发条件**：DB 事务失败（如唯一约束冲突被 Ack），但 ES 写入成功
- **后果**：ES 中有 DB 中不存在的消息。用户搜索到结果但无法加载内容。
- **对比**：钉钉用 CDC binlog 驱动 ES，保证 ES 是 DB 的严格子集。

### 3.2 MAJOR 级（影响可靠性和运维安全）

#### M1. 跨实例 PushBatch 无 outbox 保护

- 覆盖 B3 的另一方面：即使 channel 可达，RPC 超时/失败也只有日志，消息永久丢失
- 需要类似 PushOutbox 的机制，但 outbox 归属于消息本身而非推送通道，设计上更复杂

#### M2. OnlineRoute 崩溃残留

- **位置**：`push_server.h:507-515`（close handler 调用 unbind）
- 非正常关闭（kill -9 / OOM / segfault）时 unbind 不被调用
- OnlineRoute SET 残留指向死实例的条目，TTL 60s 后才过期
- 期间 `onPushMessage:191` 的 `instances()` 查询返回死实例，重定向失败

#### M3. 心跳重传耦合 Message 服务

- **位置**：`push_server.h:282-348`
- `_on_heartbeat_resend` 通过 `GetOfflineMsg` RPC 获取消息内容
- Message 服务不可用时，心跳无法补送消息，unacked ZSET 持续膨胀
- Push 服务应有本地 short-term 消息缓存或 mirror

#### M4. 限流在成员校验之前

- **位置**：`transmite_server.h:127-138` 在 `transmite_server.h:148-188` 之前
- 攻击者可用不存在的 session_id 消耗限流配额
- 应先校验会话有效性，再扣减限流计数

#### M5. RateLimiter 固定窗口边界问题

- **位置**：`data_redis.hpp:474-486`
- 窗口边界可放行 ≈2N 请求（代码注释也已承认）
- DoS 场景下防御力不足

#### M6. 单 MQClient ev_loop——消费与发布竞争

- **位置**：`rabbitmq.hpp:90-104`
- 同一 `ev_loop` 线程处理 发布确认回调 + 消费回调
- Redis 超时时 publish_confirm 回调阻塞 → 阻塞所有消息消费
- Message 服务使用同一个 MQClient 做 push_queue 发布 + db_queue/es_queue 消费，存在 head-of-line blocking

#### M7. 大群路径无增量同步

- **位置**：`message_server.h:196-199`（大群走 `recent_by_seq`）
- 读扩散大群的 `GetOfflineMsg` 按 user_seq 拉取 → 大群不写 timeline → 拉不到
- 大群客户端实际需要按 `(session_id, session_seq)` 做增量同步，但当前接口不支持

#### M8. GetOfflineMsg 响应顺序不对齐

- **位置**：`message_server.h:363-374`
- Timeline 查询按 user_seq 排序；`select_by_ids` 按 seq_id（session_seq）排序
- 代码用 mid_to_user_seq map 做查找映射，但不重新排序
- 客户端收到的消息列表顺序不是严格的 user_seq 顺序

### 3.3 MINOR 级（影响体验和运维）

#### m1. 无消息 TTL / 归档策略
- message 表和 user_timeline 表无限增长
- 无冷热分离，无自动清理

#### m2. 文件二进制内联在历史消息查询中
- `GetHistoryMsg` / `GetRecentMsg` 拉取完整文件二进制
- 客户端通常只需要缩略图/元信息，带宽浪费

#### m3. WorkIdAllocator abort 策略过于激进
- `transmite_server.h:336` lease_lost → `std::abort()`
- 可以尝试重新分配新的 worker_id 而不是自杀

#### m4. publish_confirm 超时未设置
- `rabbitmq.hpp` 中 publish 无超时控制
- broker 无响应时回调永久挂起

---

## 4. 风险矩阵

| # | 缺陷 | 严重度 | 触发概率 | 影响 | 已有设计？ |
|---|------|--------|----------|------|-----------|
| B1 | SeqGen 无回填 | BLOCKER | 低（Redis 重启时 100%） | 消息丢失 | 否 |
| B2 | Members DEL+warm | BLOCKER | 中（变更+消息并发） | 成员过时 | 是（缓存改造） |
| B3 | PushBatch 无声丢失 | BLOCKER | 低（实例崩溃时 100%） | 推送丢失 | 否 |
| B4 | ES 幽灵数据 | BLOCKER | 低（DB 失败+ES 成功） | 搜索不准确 | 是（ES 改造） |
| M1 | PushBatch 无 outbox | MAJOR | 低 | 推送丢失 | 否 |
| M2 | OnlineRoute 残留 | MAJOR | 中 | 推送延迟 | 否 |
| M3 | 心跳耦合 Message | MAJOR | 低 | 补送失败 | 否 |
| M4 | 限流顺序 | MAJOR | 中 | DoS 漏洞 | 否 |
| M5 | 限流窗口边界 | MAJOR | 低 | DoS 防御减弱 | 否 |
| M6 | ev_loop HOLB | MAJOR | 低（慢 Redis 时） | 消费延迟 | 否 |
| M7 | 大群无增量同步 | MAJOR | — | 功能缺失 | 否 |
| M8 | msg 顺序不对齐 | MAJOR | 高（每次增量拉取） | 客户端展示乱序 | 否 |

---

## 5. 架构评分总览

| 维度 | ChatNow | 评分 | 说明 |
|------|---------|------|------|
| 消息摄入 | 幂等+限流+seq | ★★★☆☆ | 框架正确，回填缺失是关键短板 |
| ID 生成 | Snowflake + Redis INCR | ★★★★☆ | 双 ID 策略成熟，Redis 重启是唯一软肋 |
| 消息存储 | 写/读扩散自适应 | ★★★★☆ | 阈值硬编码但策略正确 |
| ES 索引 | FANOUT 独立消费 | ★★☆☆☆ | 一致性无保证，已在改造中 |
| 推送链路 | WS + 跨实例 + ACK | ★★★☆☆ | 框架正确，跨实例无容错是关键短板 |
| 缓存策略 | Cache-Aside（部分） | ★★☆☆☆ | 无防击穿/雪崩，DEL 回退，已在改造中 |
| 可靠性 | MQ + outbox + 重传 | ★★★☆☆ | 内部链路有 outbox，跨边界缺失 |
| 多实例 | Redis 路由 + 租约 | ★★★☆☆ | 残留清理和故障转移不完善 |
| 可观测性 | LOG 为主 | ★★☆☆☆ | 无 metrics、无 tracing、无告警集成 |

---

## 6. 剩余待设计项

基于以上分析，以下缺陷**尚未有设计文档**：

| 优先级 | 缺陷 | 建议设计方向 |
|--------|------|------------|
| **P0** | B1 SeqGen 回填 | Message 服务启动时从 DB 回填所有 session/user seq |
| **P0** | B3 PushBatch 跨实例 outbox | 给 cross-instance push 路径加 outbox（类似 PushOutbox 但 key 粒度不同） |
| **P1** | M2 OnlineRoute 崩溃清理 | Push 实例心跳注册 + 定时扫描僵死实例清理路由表 |
| **P1** | M3 Push 本地消息缓存 | Push 服务维护最近 N 条消息的 ring buffer，心跳重传不依赖 Message RPC |
| **P1** | M4 限流顺序 | 将限流移到会话/成员校验之后 |
| **P1** | M6 MQClient 发布/消费分离 | 为 publish 和 consume 使用独立 MQClient 实例 |
| **P1** | M7 大群增量同步 | 为大群提供基于 `(session_id, seq_id)` 的 GetOfflineMsg 变体 |
| **P2** | M5 限流算法升级 | 令牌桶或滑动窗口替换固定窗口 |
| **P2** | M8 GetOfflineMsg 响应排序 | 在 SQL 层或应用层按 user_seq 排序后再返回 |
| **P3** | m1 消息归档 | TTL 策略 + 定时任务归档老消息到冷存储 |
| **P3** | m2 文件懒加载 | 历史消息只返回 file_id，客户端按需拉取 |
