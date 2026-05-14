# ChatNow 高并发 / 高可用 / 高性能：缓存一致性与消息幂等/顺序性深度分析

> **日期**: 2026-05-13
> **基线**: main @ `55decee`（含 `fix/v2.0-blockers-and-cleanup` 全部修复）
> **分析范围**: 核心消息链路（Transmite → MQ → Message → Push → Client），涵盖 8 个服务、11 个 proto 文件、约 45 个源文件

---

## 0. 分析结论（TL;DR）

当前架构在"修完 v2.0 的 9 个 BLOCKER/MAJOR"之后已经是一条**能跑通的链路**，但在高并发 / 高可用维度上仍有 **5 个硬伤**：

| # | 硬伤 | 严重程度 | 一句话 |
|---|---|---|---|
| 1 | SeqGen 启动不回填，Redis 数据丢失 → seq 撞号 | **BLOCKER** | backfill 方法存在但从没被调用过 |
| 2 | Publisher mandatory=false，Transmite 早于 Message 启动 → FANOUT 静默丢消息 | **BLOCKER** | broker 收到 ≠ queue 收到 |
| 3 | 跨实例 PushBatch 失败仅 LOG_WARN | **MAJOR** | 远端用户消息丢失且不可恢复 |
| 4 | Members 缓存用全量 invalidate 非增量维护 | **MAJOR** | 失效后首次发消息需全量 RPC，大群延迟陡增 |
| 5 | OnlineRoute TTL 60s，push 实例 crash 后 60s 内跨实例路由仍指向死节点 | **MINOR** | 60s 窗口内的消息丢失（依赖 unacked 重传补） |

好消息是除了 #1 和 #2，其余问题都有现有机制兜底（unacked 重传、outbox reaper、DLX），系统具有**最终一致性**，只是恢复时间窗口偏长。

---

## 1. 系统架构速览（消息链路）

```
Client
  │  HTTP POST /service/message_transmit/new_message
  ▼
Gateway (HTTP :9000, 无状态)
  │  鉴权 → brpc 转发
  ▼
Transmite (Ingest)
  ├─ ① SelectByClientMsg         幂等去重（命中即返）
  ├─ ② RateLimiter.allow_*      用户级+会话级限流
  ├─ ③ Members.list             成员列表（Redis 缓存优先）
  ├─ ④ SnowflakeId.Next()       全局 message_id
  ├─ ⑤ SeqGen.next_session_seq  会话内 seq_id (Redis INCR)
  ├─ ⑥ SeqGen.next_user_seq_batch  每成员 user_seq (Redis INCR)
  └─ ⑦ Publisher.publish_confirm → chat_msg_exchange (FANOUT)
      │
      ▼
RabbitMQ (FANOUT → msg_queue_db + msg_queue_es)
      │
      ▼
Message Service (Store)
  ├─ DB Consumer:
  │   ├─ 单事务: INSERT message + (写扩散群) user_timeline 批量
  │   ├─ publish_confirm → msg_push_queue
  │   └─ 失败 → PushOutbox enqueue → reaper 重投
  ├─ ES Consumer: 仅文本消息进索引
  └─ RPC Service: GetOfflineMsg / GetRecentMsg / UpdateAckSeq / ...
      │
      ▼
Push Service (WebSocket :9001)
  ├─ onPushMessage:
  │   ├─ UnackedPush.push      入 ZSET 等 ACK
  │   ├─ _local_send           本机 WS 直推
  │   └─ PushBatch             跨实例异步转发
  ├─ onClientNotify:
  │   ├─ MSG_PUSH_ACK → zrem + UpdateAckSeq
  │   └─ CLIENT_HEARTBEAT → peek_due → GetOfflineMsg → 重发
  └─ OnlineRoute: Redis SET<instance> 路由表
```

---

## 2. 缓存一致性问题深度分析

系统中有 **5 类 Redis 缓存** 参与消息链路，每类的一致性保证程度不同。

### 2.1 会话成员缓存 `im:members:{ssid}`

**读写路径：**

```
读: Transmite 每条消息 → Members.list(ssid)
   命中 → 直接用
   未命中 → ChatSession.GetMemberIdList RPC → Members.warm(ssid, TTL=30min)

写: ChatSession 成员变更 → Members.invalidate(ssid)  // 全量 DEL
```

**一致性分析：**

| 场景 | 行为 | 风险 |
|---|---|---|
| 加人 → 发消息 | invalidate → 下次 warm(全量) | 正确（下次消息必然看到新成员） |
| 踢人 → 发消息 | invalidate → 下次 warm(全量) | 正确（被踢者不会再被推） |
| 加人 → 加人 → 发消息 | 两次 invalidate | 正确，但两次全量 RPC |
| 并发：加人 + 发消息 | invalidate 与 warm 无顺序保证 | **可能**: warm 先于 invalidate → 旧集合覆盖新集合 → 加人后的首条消息漏掉新成员 |

**竞态场景详细时序：**

```
Transmite-A (发消息)              ChatSession (加成员)
  │                                  │
  ├─ Members.list → 命中旧集合       │
  │   (不含新成员 U2)                ├─ DB insert member
  │                                  ├─ Members.invalidate()  → DEL key
  ├─ Members.warm(旧集合)  ────────────────────────────────────────────→ SET key = {U1}
  │                                  │
  ▼                                  ▼
  消息只发给 U1，U2 收不到！
```

**为什么概率低但可能发生**: Transmite 的 `list()` 和 `warm()` 之间隔了 RPC 调用（GetUserInfo、SeqGen 等），给了 ChatSession 插入 invalidate 的窗口。

**当前缓解**: TTL 只有 30 分钟，过期后自然修复；且 invalidate 后的下一条消息会 cold-RPC 拉全量。

**建议修复**: 
- 方案 A（最小改动）：invalidate 改为 bump version — `SET im:members:v:{ssid} {new_version}`，warm 时 CAS check version。warm 前先读 version，warm 后如果 version 变了，再 invalidate 自己。代价是 2 次额外 Redis 操作。
- 方案 B（推荐）：ChatSession 改为增量维护 — 不用 invalidate，而是直接 `sadd` / `srem` + `expire` 续期。这样就不存在"全量覆盖"的竞态窗口。当前 Members 类已有 `add()` / `remove()` 方法（data_redis.hpp:395-402），但 ChatSession 端没用它们。
- 方案 C（过度设计）：写扩散群发消息前不做成员列表 RPC，而是由 ChatSession 将成员变更事件发到 Redis Stream，Transmite 订阅消费维护本地缓存。**不推荐**，太复杂。

### 2.2 在线路由表 `im:online:{uid}`

**读写路径：**

```
写: Push.onOpen  → OnlineRoute.bind(uid, instance, TTL=60s)
    Push.onClose → OnlineRoute.unbind(uid, instance)
    Push.heartbeat → OnlineRoute.touch(uid)

读: Push.onPushMessage → OnlineRoute.instances(uid) → 跨实例路由
```

**一致性分析：**

| 场景 | 行为 | 风险 |
|---|---|---|
| 正常上下线 | bind/unbind 即时 | 正确（秒级一致性） |
| 客户端断网（无 close frame） | TTL 60s 后自然过期 | 60s 窗口内跨实例转发到 offline 用户 |
| Push 实例 crash | 无 unbind 调用 | **60s 窗口内路由到死节点** |
| 两实例同时 bind 同一 uid | Redis SADD 幂等 | 正确（SET 会去重） |

**Push 实例 crash 场景的完整链路：**

```
Push-A 实例 crash（持有 U1 的 WS 连接）
  │
  ├─ OnlineRoute 仍记录 U1 → Push-A (TTL 还剩 50s)
  │
  ├─ 此时一条群消息到达 Push-B
  │   ├─ _local_send(U1) → 不在本机 → remote_uids
  │   ├─ OnlineRoute.instances(U1) → ["Push-A"]
  │   └─ PushBatch → Push-A → brpc 超时 / 连接拒绝 → LOG_WARN (消息丢失)
  │
  └─ 50s 后 TTL 过期 → OnlineRoute 清理 → 下一条消息不再路由到 Push-A
```

**当前缓解**: UnackedPush 记录了 user_seq，心跳重发会补。但心跳周期 × 重发延迟 × TTL 窗口 = 最多约 65s 的消息丢失窗口。

**建议修复**:
- PushServer::start() 的 shutdown 路径里主动 `OnlineRoute.unbind` 所有本实例持有的 uid（需要遍历 _connections）
- 或者将 TTL 降到 15s（心跳间隔 ≤ 10s），则最差窗口 ≤ 25s
- 或者：跨实例 PushBatch 失败时把消息 payload 入 UnackedPush（需要扩展 UnackedPush 存完整 payload）

### 2.3 序号生成器 `im:seq:ssid:{ssid}` / `im:seq:uid:{uid}`

**这是最严重的缓存一致性问题。**

**当前行为：**

```
Transmite: seq_id = SeqGen.next_session_seq(ssid)  // Redis INCR
Message DB Consumer: INSERT message(seq_id)         // 落库

如果 Redis 重启 / 数据丢失:
  INCR im:seq:ssid:{ssid} → 返回 1
  但 DB 中已有 seq_id=1,2,3,...,9999 的消息
  → 新消息 seq_id 从 1 开始 → uk_session_seq 唯一约束冲突 → 消息丢失！
```

**证据**: `SeqGen::backfill_session()` 和 `backfill_user()` 方法已经实现了（data_redis.hpp:237-254），但在整个代码库中 **从未被调用**（grep 无结果）。

**影响范围**:
- `session_seq` 冲突 → `uk_session_seq` 唯一索引冲突 → `onDBMessage` 走 `object_already_persistent` catch → **消息被静默丢弃**（ConsumeAction::Ack）
- `user_seq` 冲突 → `uk_user_seq` 唯一索引冲突 → user_timeline 插入失败 → 事务回滚 → NackRequeue → 无限重试或 DLX

**建议修复（高优先级）**:
1. MessageServer::start() 启动时对每个已知 session 执行 `backfill_session(ssid, MAX(seq_id) from DB + 1)`
2. 或者更简单：Transmite 申请 seq 时如果值 < DB 中的 max，自动跳到 max+1
3. 最低成本方案：运维侧确保 Redis 持久化（AOF + RDB），但这不能 100% 防止

### 2.4 未 ACK 缓冲 `im:unack:{uid}`

**一致性分析：**

```
Push.onPushMessage  → UnackedPush.push(uid, user_seq, now)   // ZADD
Client ACK          → UnackedPush.ack(uid, user_seq)          // ZREM
Push.heartbeat      → UnackedPush.peek_due → GetOfflineMsg → 重发
Push.heartbeat      → UnackedPush.bump_score                  // ZADD XX 续期
```

每个 key 有 TTL = 7 天。整体一致性问题不大 —— 即使 UpdateAckSeq 异步 RPC 失败，下一条 ACK 带更大的 user_seq 会通过 GREATEST 纠正。唯一风险是 **整 key TTL 过期**，但 7 天远超实际需要（正常 ACK 在秒级完成）。

### 2.5 最近消息预览缓存 `im:last:{ssid}`

类 `LastMessage` 已定义但**未在任何服务中实际使用**。目前会话列表的"最近一条消息"仍然通过 RPC 查 DB（从 `list_ordered_by_user` 视图走）。这是一个纯粹的优化点，非一致性问题。

---

## 3. 消息幂等性与顺序性深度分析

### 3.1 client_msg_id 幂等机制

**当前实现（两层防护）：**

```
第一层（Transmite）：SelectByClientMsg RPC 预查
  - 同步 brpc 调用，命中 → 直接返回旧 msg，不创建新 ID、不写 MQ
  - 未命中 → 继续正常路径

第二层（DB Consumer）：uk_client_msg(user_id, client_msg_id) 唯一索引
  - INSERT 冲突 → object_already_persistent 异常 → ConsumeAction::Ack（幂等丢弃）
```

**并发下的行为分析：**

```
Transmite-A                     Transmite-B
  │                               │
  ├─ SelectByClientMsg → 不存在    ├─ SelectByClientMsg → 不存在
  ├─ Snowflake.Next() = mid_A     ├─ Snowflake.Next() = mid_B
  ├─ publish_confirm              ├─ publish_confirm
  │                               │
  ▼                               ▼
       DB Consumer 并发插入:
       - mid_A 先到 → INSERT 成功
       - mid_B 后到 → uk_client_msg 冲突 → Ack（丢弃）
       
       结果：只有一条消息落库，但两个客户端都收到了"发送成功"。
```

**评价**: 这是正确且合理的幂等语义。"客户端收到 success ≠ DB 已落库"是异步消息系统的固有特性。客户端应按 `client_msg_id` 自行去重（在本地"发送中"状态下展示一条即可）。

**无竞态问题的原因**: 即使 Transmite 的 SelectByClientMsg 预查漏了（两个请求几乎同时到达），DB 唯一索引保证最终只有一条落库。

### 3.2 seq_id 单调性与"留洞"问题

**seq_id 的生成时机 vs 落库时机：**

```
Transmite: seq_id = Redis INCR (同步，在 publish_confirm 之前)
Message DB Consumer: INSERT message(seq_id) (异步，在 MQ 消费之后)
```

**这意味着 seq_id 代表"消息到达 Transmite 的顺序"，而非"消息落库的顺序"。**

**留洞场景：**

```
Transmite 收到消息 M1 → seq_id=100 → publish_confirm → broker Ack
Transmite 收到消息 M2 → seq_id=101 → publish_confirm → broker Ack

DB Consumer:
  - 先收到 M2 (seq_id=101) → INSERT 成功
  - M1 (seq_id=100) 还在队列中（或 DB Consumer 失败 NackRequeue）
  
此时客户端拉 GetRecentMsg / GetOfflineMsg:
  - 看到 seq_id=101，但 100 不存在 → "留洞"
```

**留洞的后果：**

1. **客户端展示**: seq_id 不连续不代表丢消息，因为正常的并发写入就是不连续的。雪花 ID 天生就是非连续的，客户端不能依赖连续性判断是否丢消息。
2. **GetOfflineMsg 增量同步**: 客户端传 `last_user_seq` 而非 `last_seq_id` 来拉增量。user_seq 也是 INCR 生成，同样有留洞。但 GetOfflineMsg 走 `user_seq > last_user_seq` 查询，拿到所有 > last 的 timeline 行，中间缺失的 seq 对应的消息可能还没落库。has_more=true 提示客户端继续拉。
3. **UnackedPush**: 存储的是 `user_seq` 而非 `seq_id`。客户端 ACK 按 user_seq，不是连续 ack。

**评估**: 留洞在当前设计下**不是问题**，因为：
- 客户端不应依赖 seq 连续性判断完整性
- 增量同步走 `user_seq > last_user_seq` 而非 `seq_id BETWEEN a AND b`
- ACK 走 user_seq 单点确认

**但有一个边缘问题**: 在大群读扩散场景（≥200 成员），消息不写 user_timeline → 没有 user_seq → `GetOfflineMsg` 走不了 user_seq 增量路径。**大群成员只能通过 GetRecentMsg 按时间拉，无法按 user_seq 做增量同步。**

### 3.3 user_seq 的 ACK 语义

**当前链路：**

```
Push→Client: 推送 NotifyNewMessage { user_seq=N }
Client→Push: MSG_PUSH_ACK { user_seq=N }
Push:        1) UnackedPush.zrem(uid, N)
             2) 异步 UpdateAckSeq(uid, ssid, N) → DB UPDATE GREATEST(last_ack_seq, N)
```

**乱序 ACK 场景：**

```
客户端收到: user_seq=5, 7, 8  (6 未收到——网络丢包或 Push 失败)
客户端 ACK: 5, 7, 8

服务端:
  UnackedPush: zrem 5,7,8 → 剩 {6}
  UpdateAckSeq: GREATEST(last_ack_seq, 5) → 5
                GREATEST(last_ack_seq, 7) → 7
                GREATEST(last_ack_seq, 8) → 8
  
  下次心跳: peek_due(uid) → {6} → GetOfflineMsg → 重发 6 ✓
```

**正确性**: ACK 语义是"单点确认"而非"累积确认"。每个 user_seq 独立 ACK，不依赖连续性。这正确避免了"ack 7 导致 server 认为 1-7 已送达"的传统 TCP 式累积 ACK 问题。

**UpdateAckSeq 的弱一致性**: 异步 brpc 调用，失败仅 LOG_WARN。这意味着 `last_ack_seq` 可能短暂落后于实际 ACK 状态。但下一条 ACK 带更大的 seq，GREATEST 会自动追上。最终一致，无数据丢失风险。

### 3.4 Push 端去重 —— 客户端可能收到重复推送吗？

**可能收到重复的场景：**

1. 正常推送 + 心跳重发：正常推送送达，但 ACK 丢失 → user_seq 仍在 unacked → 心跳触发重发 → 客户端收到同一条消息两次
2. 跨实例 PushBatch + 本机重连：用户从 Push-A 切换到 Push-B，Push-A 还没 unbind → 两边都可能推

**客户端去重责任**: 客户端必须按 `(chat_session_id, user_seq)` 去重。同一个 user_seq 的消息只展示一次。服务端不负责推送去重。

**评估**: 这是标准 IM 设计。推送是 at-most-once + at-least-once 的混合（正常推送是 at-most-once，重传是 at-least-once），客户端做 exactly-once 展示。

---

## 4. 高并发 / 高性能瓶颈分析

### 4.1 单 DB Consumer 的吞吐天花板

**当前**: `onDBMessage` 回调在 MQ ev 线程中同步执行（含 DB 事务 + publish_confirm）。prefetch=64 限制了内存堆积，但**消费是单线程串行的**。

**瓶颈量化**:
- 假设单条消息处理 5ms（DB insert message + 200 行 timeline + publish_confirm）
- 单线程上限 = 200 msg/s
- 这在普通 IM 场景足够（10 万 DAU，峰值 50 msg/s）
- 但在热点会话（如大型直播群聊）可能成为瓶颈

**现有缓解措施**:
- 大群（≥200）走读扩散，不写 user_timeline → 减少事务体积
- PushOutbox reaper 异步处理 push_queue 投递失败 → 不阻塞消费

**建议方向**（仅当实际压测发现瓶颈时）:
- 按 session_id hash 分片到多个 queue，每个 queue 一个 consumer 线程
- Timeline 写入改为异步批处理（攒 100 条或 100ms flush 一次）

### 4.2 SeqGen Redis 热点

**热点分析**: `im:seq:ssid:{ssid}` 被每条消息访问一次 `INCR`。单 Redis 实例的 INCR 性能约 10 万/s。对于单个会话此能力远够（正常人聊天不可能 10 万 msg/s）。真正的热点在 `im:seq:uid:{uid}` —— 如果某个用户参与 200 个群、每个群每秒 10 条消息 = 2000 INCR/s，仍然远低于 Redis 能力。

**结论**: **当前不需要分片 SeqGen**。等有百万级并发再考虑按 uid hash 到不同 Redis 实例。

### 4.3 Fat Message (InternalMessage) 体积

**当前**: InternalMessage 携带完整成员列表（member_id_list + user_seqs）。200 人群 = 约 20KB（每个 uid 约 50 字节 × 200 × 2）。经过 MQ 两次（chat_msg_exchange + msg_push_queue）。

**评估**: 200 人 × 20KB = 4MB 内存占用（MQ 队列中）。对于 RabbitMQ 这是可以接受的。MQ 瓶颈通常在网络 IO 而非内存。

**不需要优化**，除非群规模到 5000+ 人。

### 4.4 Members 缓存命中率

**冷启动场景**: 服务全重启后，所有会话的 members 缓存为空 → 每条消息的首条都要 RPC 拉成员列表 → 延迟增加 5-20ms。

**热路径**: 30 分钟 TTL 内，活跃会话的缓存命中率接近 100%（只有成员变更时 invalidate 导致 miss）。

**评估**: 可接受。预热（启动时扫描活跃会话预加载缓存）是锦上添花，非必需。

---

## 5. 风险矩阵（概率 × 影响）

| # | 风险 | 概率 | 影响 | 等级 | 现有兜底 |
|---|---|---|---|---|---|
| 1 | Redis 数据丢失 → seq 撞号 | 低（运维故障）| 致命（消息丢失）| **BLOCKER** | 无 |
| 2 | Transmite > Message 启动序错 → 静默丢消息 | 中（运维操作) | 致命（消息丢失）| **BLOCKER** | 无 |
| 3 | Members 缓存竞态 → 漏推 | 低（并发窗口窄）| 中（单条消息漏人）| **MAJOR** | TTL 过期自修复 |
| 4 | Push 实例 crash 路由残留 | 低（crash 罕见）| 中（60s 内消息丢失）| **MAJOR** | Unacked 重传补 |
| 5 | 跨实例 PushBatch RPC 失败 | 中（网络抖动）| 中（远端用户丢消息）| **MAJOR** | Unacked 重传补 |
| 6 | Publisher mandatory=false | 中（运维配置错）| 高（静默丢消息）| **MAJOR** | 无（7.1 启动顺序文档规避） |
| 7 | OnlineRoute 多实例 bind 竞态 | 极低 | 极低（多实例发重复）| **MINOR** | Client 去重 |
| 8 | seq_id 留洞 | 中（高并发常态）| 低（客户端不应依赖）| **MINOR** | GetOfflineMsg 走 user_seq |
| 9 | 限流早于 Members 校验 | 低（需伪造 uid）| 中（DoS 其它用户）| MAJOR（继承） | 无（见原 REVIEW） |
| 10 | fallback worker_id 撞号 | 低（Redis 不通）| 致命（message_id 重复）| MAJOR（继承） | 无（见原 REVIEW） |

---

## 6. 推荐改进方案（按投入产出比排序）

### P0 — 必须立即做

#### 6.1 SeqGen 启动回填（风险 #1）

**问题**: `backfill_session/user` 未被调用。

**改动**（约 20 行）: 在 `MessageServerBuilder::make_rpc_object()` 末尾（或 `MessageServer::start()` 开始），遍历所有活跃 session，从 DB `SELECT MAX(seq_id) FROM message WHERE session_id = ? GROUP BY session_id`，对每个 session 调 `_seq_gen->backfill_session(ssid, max_seq_id)`；同理对 user 维度调 `backfill_user`。

同时也应该在 Transmite 侧做防御：`next_session_seq` 返回的值异常小（远小于已知的 max）时，报警并跳过该消息。

#### 6.2 Publisher mandatory=true（风险 #2, #6）

**问题**: `publish_confirm` 不设置 mandatory flag，broker 收到的消息若无 queue 绑定，FANOUT 静默丢弃。

**改动**:
- `MQClient::publish_confirm` 增加 `mandatory` 参数（或默认 true）
- 设置 `basic.return` 回调，收到 return → 回调 `PublishStatus::Lost`
- Transmite 收到 Lost → `err_response("消息投递失败，请重试")`，不返回 success
- MessageServer start 时先声明 queue + bind，再让 Transmite 发布

### P1 — 近期应做

#### 6.3 Members 缓存改为增量维护（风险 #3）

**问题**: ChatSession 成员变更用 `invalidate()`（全量 DEL），与 Transmite 的 `warm()` 存在写覆盖竞态。

**改动**（约 30 行）: 
- `AddChatSessionMember` 末尾：对每个新增 member 调 `_members_cache->add(ssid, uid)` + `expire` 续期
- `RemoveChatSessionMember` 末尾：对每个被移除 member 调 `_members_cache->remove(ssid, uid)`
- `QuitChatSession` 末尾：调 `_members_cache->remove(ssid, uid)`
- 保留 `invalidate()` 调用作为兜底（在创建/解散会话时全量失效）
- Transmite 的 `warm()` 改为 `SADD` 追加（而非覆盖全量）—— 但这要求 `warm()` 的语义改变，需要仔细处理首次缓存填充的场景

**更简单的替代方案**: 在 ChatSession 端，invalidate 后追加一个 `sleep(0)` 或用 Redis `WATCH` 事务来避免竞态。但这治标不治本。

#### 6.4 跨实例 PushBatch 失败入 outbox / unacked payload（风险 #4, #5）

**问题**: 跨实例 PushBatch 失败只 LOG_WARN，消息丢失且 unacked 不存 payload。

**改动**:
- 方案 A（最小）：PushBatch 失败时把 uid+payload 写入 UnackedPush 的特殊 key（如 `im:unack:payload:{uid}` → HSET），心跳重发时直接取 payload 而不用调 GetOfflineMsg RPC
- 方案 B（标准）：跨实例失败入 PushOutbox（与 push_queue 投递失败同一路径），reaper 重试
- 推荐方案 A 改动最小（约 40 行），且消除了心跳重发对 message 服务的同步 RPC 依赖

#### 6.5 Push 实例优雅关停清理 OnlineRoute（风险 #4）

**问题**: Push crash 后 OnlineRoute 残留 60s。

**改动**（约 15 行）: 在 `PushServer::start()` 的关停路径中（`_ws_server->stop()` 之后），遍历 `_connections` 中所有 uid，调 `_online_route->unbind(uid, _instance_id)`。

### P2 — 按需做

#### 6.6 大群读扩散的增量同步路径

**问题**: 大群（≥200 人）不写 user_timeline → GetOfflineMsg 查不到这些消息 → 大群成员无法做基于 user_seq 的增量同步，只能定期 GetRecentMsg。

**改动**: `GetOfflineMsg` 查询时 union 两条路径：
1. user_timeline WHERE user_seq > last_user_seq（小群路径）
2. message WHERE session_id IN (用户的大群列表) AND message_id > last_message_id（大群读扩散路径）

需要客户端传 `last_message_id`（雪花 ID）作为大群路径的游标。

#### 6.7 心跳重发消除对 Message 服务的同步 RPC 依赖

**当前**: 心跳触发 → GetOfflineMsg RPC → 拿到消息体 → 重发。如果 message 服务故障，重发链路也断。

**改进**: UnackedPush 存 `(user_seq, serialized_payload)` 而非只存 `user_seq`。心跳重发直接从 Redis 取 payload。代价：每条未 ack 消息的 payload 在 Redis 多存一份（约 2-5KB × N）。对 99% 在 5 秒内 ack 的场景，Redis 额外内存占用可忽略。

### P3 — 锦上添花

- **SeqGen 分片**: 当单 Redis 实例 INCR 超 5 万/s 时，按 `hash(ssid) % N` 分到不同 Redis 实例
- **DB Consumer 多线程**: 按 `hash(session_id) % N` 投递到多个 queue，每个 queue 独立线程消费
- **LastMessage 缓存接入**: 写消息时更新 `im:last:{ssid}`，会话列表直接用缓存而非查 DB 视图
- **L1 监控（brpc /vars /status 暴露）**: 1 天工作量，连接数 / QPS / 延迟 / 错误率一目了然

---

## 7. 不建议做的（过度设计）

| 方案 | 为什么不建议 |
|---|---|
| Members 缓存走 Redis Stream 订阅变更 | 引入新组件（Stream）、消费位点管理、重连重订阅，复杂度远超收益 |
| 消息落库前加分布式锁保证 seq 连续性 | seq 留洞不影响正确性；分布式锁大幅降低吞吐 |
| Kafka 替换 RabbitMQ | 当前规模不需要；迁移成本高、运维复杂度大；FANOUT exchange 在 RabbitMQ 有天然优势 |
| Sequencer 服务统一授号 | 单点瓶颈；Redis INCR 已经满足单调性需求；留洞问题不应通过架构解决 |
| E2EE | 产品定位未明确；需客户端配合；属于功能需求非架构需求 |
| 消息聚合服务（Mention/Reaction/Read Receipt 独立表） | 当前阶段无这些 feature；等有需求再拆不迟 |

---

## 8. 总结

当前 ChatNow v2.0 的核心消息链路在正确性上已达到"能跑"水平。本分析识别出 **5 个硬伤**，其中 2 个是 BLOCKER（SeqGen 不回填 + Publisher 不设 mandatory），3 个是 MAJOR（Members 缓存竞态 + 跨实例推送失败无兜底 + OnlineRoute crash 残留）。

**最近两周建议路线**（约 5 人天）：

1. Day 1: SeqGen 启动回填 + Publisher mandatory（P0，消除静默丢消息）
2. Day 2-3: Members 缓存增量维护 + 跨实例推送失败入 unacked payload（P1）
3. Day 4: Push 优雅关停 + L1 监控（brpc /vars /status 暴露）
4. Day 5: 端到端压测验证（构造：Redis 重启 / Push crash / Message 重启 / 大群高并发）

做完以上，系统在"高并发 / 高可用 / 高性能"三个维度上的短期目标就达成了：**消息不丢、ACK 闭环、缓存最终一致、故障可观测**。
