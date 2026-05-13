# 消息链路：缺陷分析与高并发/高可用改造

> **范围**：客户端发送一条消息 → 网关 → 转发 → MQ → 消息存储 → 推送给在线接收者；以及上线后的离线/增量同步。
> **目的**：标定当前实现里被高并发流量撕开的真实裂缝，并给出可落地的演进路径。
>
> 本文不是一份重写计划，而是按"现状 → 缺陷 → 改法"逐个把链路上的关键节点剖开。

---

## 0. 当前链路概览

```
                                                                              ┌──── 文件服务（put）
                                                                              │
客户端                                                                          ▼
  │  HTTP POST /service/message_transmit/new_message                  ┌─→ Message DB 消费者
  ▼                                                                   │   写 message + user_timeline (一个事务)
Gateway                                                              │
  │  鉴权(Redis Session) → 注入 user_id → brpc 转发                    │
  ▼                                                                   │
Transmite                                                            │
  ① 并行 RPC: User.GetUserInfo + ChatSession.GetMemberIdList          ├─→ Message ES 消费者
  ② 雪花生成 message_id, 组装 InternalMessage（含成员列表）            │   仅文本消息进 ES
  ③ publish_confirm 异步投递到 RabbitMQ FANOUT 交换机                  │
  ④ Broker ACK 后 done->Run() 返回 success + 完整消息体 + target list  ▼
  │                                                                   RabbitMQ
  ▼                                                                   chat_msg_exchange (FANOUT)
Gateway                                                                ├─ msg_queue_db
  ⑤ 遍历 target_id_list，对在线用户通过 WebSocket 推送                  └─ msg_queue_es
     CHAT_MESSAGE_NOTIFY
```

**关键代码定位**

| 模块 | 位置 |
|---|---|
| HTTP 入口 | `code/server/gateway/source/gateway_server.h:1342` `NewMessage` |
| 转发服务  | `code/server/transmite/source/transmite_server.h:37` `GetTransmitTarget` |
| DB 消费者 | `code/server/message/source/message_server.h:467` `onDBMessage` |
| ES 消费者 | `code/server/message/source/message_server.h:548` `onESMessage` |
| 离线同步  | `code/server/message/source/message_server.h:303` `GetOfflineMsg` |

---

## 1. 关键缺陷清单（按危险等级排序）

### 🔴 P0 — 错误或致命

#### 1.1 **MQ 投递成功 ≠ 消息可靠送达**
`Transmite` 在 broker `Acked` 时即返回客户端"发送成功"。这只代表 broker 收到了，**不代表消息已落库 / 已推送给接收者**。一旦 DB Consumer 失败（NackRequeue 累计），客户端早已记录"已发出"的状态，与服务端真实状态出现持续偏差。

**影响**：消息列表出现"我能看到我发的，但对方永远收不到"的鬼消息。

#### 1.2 **写库与推送是两条独立路径，没有强一致 / 顺序保证**
现状：
- `Transmite` 通过 RPC **同步**返回 target 列表给 Gateway，由 Gateway 直接 WebSocket 推
- DB Consumer 异步写库

这意味着：**接收者收到推送后立刻刷历史，可能拉不到刚刚那条消息**（DB 还没消费完）。在群聊场景概率显著。

#### 1.3 **没有 `(session_id, seq_id)` 单调序号**
当前 `message_id` 是雪花，全局递增；**会话内不连续**。带来的全链路问题：
- 客户端无法靠"本地最大 seq + 拉所有 > seq 的消息"做增量同步
- 客户端无法判断是否丢消息（雪花的间隔正常情况就是不连续的）
- 群已读未读没法用差值算（unread = max_seq - last_read_seq）

> 这正是 PR #5 schema 重构补的字段，但消息链路代码还没用上。

#### 1.4 **断网重发会产生重复消息**
请求里没有 `client_msg_id`，服务端没有去重逻辑。客户端发完 WiFi 切 4G 重发一次 → 库里两条不同 `message_id` 的相同内容消息。

> PR #5 补了 `(user_id, client_msg_id) UNIQUE` 字段，但 `Transmite` 没读没写。

#### 1.5 **Snowflake 多实例 worker_id 没做严格分配**
`transmite_server.h:190` 构造时直接用配置传入的 `worker_id`。生产环境多个 transmite 实例并行启动时，**没有任何机制保证 worker_id 唯一**。两个实例如果配置错都用 0，会撞 ID。

**影响**：`message_id` 全局唯一索引冲突 → 写库失败 → 消息丢失。

#### 1.6 **大群消息扩散无分片，写放大压垮 MQ Consumer**
1000 人群发一条消息 → DB Consumer 在一个事务里写 1 条 message + 1000 条 user_timeline。
- 单事务过大（千行 INSERT + 上层文件上传）
- 慢消费 → MQ 堆积 → 连锁阻塞
- 失败时整批 NackRequeue → 重投后**重复执行文件上传**（File 服务 PutSingleFile 不幂等）

### 🟠 P1 — 显著架构问题

#### 1.7 **群成员列表每条消息都查一次 ChatSession**
`Transmite` 每条消息都调 `GetMemberIdList`。万人群发消息 = 万次 RPC 拉同样的列表。**没有缓存层**。

#### 1.8 **推送通过 WebSocket 直推，Gateway 无水平扩展能力**
Gateway 在内存里维护 `Connection` 表（`connection.hpp`：`uid → ws_handle`）。
- 多 Gateway 实例时，A 实例上的用户的连接信息只在 A 上
- `Transmite` 返回的 target_id_list 经过 Gateway 时，本机不在线即丢失推送
- 没有跨 Gateway 的消息路由

**影响**：单 Gateway 在线连接数受限于单机内存与端口；不能水平扩。

#### 1.9 **WebSocket 推送没有 ack，丢消息只能靠"上线拉补"**
现在的策略是 fire-and-forget。客户端在收到推送途中断网，这条消息就只能靠下次上线 `GetOfflineMsg` 拉回。问题是：
- 客户端不知道自己丢了什么（没有 seq 比对）
- 上线拉补走的是 `last_message_id`（雪花 ID），无法定位"丢了哪条"

#### 1.10 **`onDBMessage` 在事务里做远程文件上传**
`message_server.h:486~503` 在写 DB 之前调 `_PutFile`（同步 RPC）。
- 持有数据库事务的同时阻塞在远程调用，连接池占用恶化
- File 服务慢一点，整个 DB Consumer 吞吐崩塌

#### 1.11 **MQ 没有 prefetch 流控**（已在 PR #7 修）
旧 `consume()` 不设 qos，慢消费者把全部未 ack 消息全拉到内存。PR #7 已加默认 prefetch=64。

#### 1.12 **没有死信队列处理路径**
`onDBMessage` 失败一律 NackRequeue。如果是格式错误等永久失败，会无限重投造成 hot spin。
- `rabbitmq.hpp` 已经定义 DLX 但只在 delayed 模式启用
- DB Consumer 应在重试 N 次后转 NackDiscard 入死信，运维人工介入

### 🟡 P2 — 性能与运维

#### 1.13 **`_PutFile` 上传失败返回 NackRequeue 会重复消费整个事务**
即使 DB 写成功了，文件上传是先于事务的 — 上传失败 NackRequeue 后再次进入 `onDBMessage`，文件会被**再次上传一次**生成新的 `file_id`，旧的成为孤儿文件。

#### 1.14 **客户端发消息走 HTTP，服务端推送走 WebSocket — 通道不对称**
HTTP 短连接每次发消息都要握手（不复用 keep-alive 时），高频群聊场景吞吐差。主流 IM 都是发送也走长连接。

#### 1.15 **没有限流 / 降级开关**
- 无单用户发送频率限制（很容易被 bot 刷消息）
- 无热点会话保护（10w 群被刷直接拖 DB）
- 无降级开关（如紧急情况下只写 timeline 不写 ES，保住主链路）

#### 1.16 **离线消息拉取只支持向后增量**
`GetOfflineMsg` 只能 `last_message_id` 之后；没有"指定时间点之前 N 条"用于翻历史。`GetHistoryMsg` 用时间窗（`onDBMessage` 写库时间≠消息发送时间，会有秒级抖动）。

---

## 2. 改造蓝图（按演进顺序，每步可独立上线）

> 原则：**优先消除丢消息和重复消息（正确性）→ 再扩吞吐（性能）→ 最后做多机房高可用**。

### 阶段 A：消息正确性（解决 1.1 ~ 1.5）

**A1. 引入会话级 seq_id（基于 Redis INCR）**

链路改造：
```
Transmite
  ① 用户/会话 RPC 并行
  ② 检查 client_msg_id 去重（select_by_client_msg）
  ③ session_seq = SeqGen.next_session_seq(ssid)   ← Redis INCR
  ④ 给会话每个成员各申请一个 user_seq             ← Redis INCR
  ⑤ 雪花生成 message_id
  ⑥ 组装胖消息（含 session_seq + 每个成员的 user_seq）
  ⑦ 投递 MQ
```

> seq 在 Transmite 申请、写在 InternalMessage 里、消费者直接落库。这样 seq 在投递成功瞬间就被认领，不依赖 DB Consumer 完成时间。
>
> Redis 重启风险：消息服务启动时执行 `bump_max_seq` 从 message 表回填 max(seq_id)+1（PR #7 的 `SeqGen::backfill_session/user` 已就位）。

**A2. 客户端去重幂等**

`NewMessageReq` 增加 `client_msg_id`（UUID）。
- `Transmite` 入口先 `select_by_client_msg(user_id, client_msg_id)`，命中则直接返回旧的完整消息（绕过雪花/seq/MQ），客户端语义不变
- DB schema 已加 unique 索引兜底

**A3. 让"消息送达"语义真正落到接收方**

把"成功"的判定从 broker ack 改成"至少 timeline 已写入 + 推送已尝试"：
- Transmite 不再 `publish_confirm` 后立即返回成功
- 改为同步等到至少**主成员（发送者自己）**的 timeline 写入完成（保证发送者 UI 一致），然后立即返回
- 群其他成员的 timeline 由后台异步扩散

或者更工程化：返回 success 后客户端**用 seq_id 做事实校验**——发送者收到 seq_id 后，下次同步若 server 端拉不到这个 seq，自动重发 (client_msg_id 会去重)。

**A4. Snowflake worker_id 自动分配**

启动时通过 Redis `INCR worker_id:transmite` 申请一个全局唯一 worker_id（mod 1024），写到本实例进程内存。
- 实例下线时 `lease` 过期，编号被释放（避免长期占用）
- 配置文件中的 worker_id 仅作"启动失败时的人工兜底"

### 阶段 B：写扩散性能（解决 1.6 ~ 1.10）

**B1. 群成员列表缓存**

`ChatSession.GetMemberIdList` 之外增加 Redis 镜像 `members:{ssid} → SET<user_id>`：
- 入群 / 退群 / 解散时主动 invalidate
- Transmite 优先读 Redis，未命中回查 RPC + 回填
- TTL 兜底 30 分钟，防数据不一致永久放大

**B2. 大群分片扩散**

DB Consumer 收到一条群消息后：
- 如果 `member_count <= 200`：直接事务写 1 + N timeline（现状）
- 否则：发出 N 条**分片消息**到分片队列 `msg_fanout_shard.{0..15}`，每条携带子列表（如 200 人/片），下游分片消费者并行写。
- 主消息（message 主表）单独写一次

减小事务体积，写延迟下降 1~2 个数量级，且失败粒度收敛到"分片"。

**B3. 推送层独立化**

引入"推送服务"`push_server`：
- Gateway 不再直接维护 `uid → ws` 表，改为把 ws 注册到推送服务
- 推送服务订阅 MQ 推送队列 `msg_push_queue`
- DB Consumer 写完 timeline 后向 `msg_push_queue` 投递
- 推送服务收到后 lookup 在线设备做下发

好处：
- Gateway 真正无状态，可水平扩
- 推送和写库解耦，延迟独立观测
- 多设备分发由推送服务统一处理（依赖 `user_device` + `DeviceSet`）

**B4. 文件上传从消费链路移除**

文件上传必须在**客户端发消息之前**完成（`PutSingleFile` 拿到 `file_id`），消息体里只带 `file_id`。
- DB Consumer 看到 `file_id` 直接落库，不再发 RPC
- 文件上传失败属于客户端问题，由客户端重试，不污染消息事务
- 同步消除 1.10 / 1.13

**B5. 推送 ACK + 重传**

WebSocket 推送加 ack：
- 推送服务发完 msg 后等待客户端 ack（带 user_seq）
- 超时未 ack 的消息进入"待重传队列"，下次客户端心跳/重连时一次性补
- 配合 `last_ack_seq` 字段（PR #5 已在 `chat_session_member`）

### 阶段 C：高可用（解决 1.11 ~ 1.16）

**C1. 死信队列 + 失败转储**

`onDBMessage` / `onESMessage` 引入失败计数：
- 头部 redelivered 标志触发重试计数
- 重试 ≥ 3 次 → 转 `NackDiscard`，进入 DLX（`rabbitmq.hpp` 已支持）
- DLX 由独立 fail-store 服务消费，落 `failed_message` 表 + 报警

**C2. 限流 + 热点保护**

- 用户级令牌桶：`Redis CL.THROTTLE im:rl:user:{uid} 5 50 60`（5 QPS 平稳，1 分钟突发 50）
- 会话级令牌桶：`im:rl:ssid:{ssid}` 同理
- 全局熔断：MQ 队列长度超阈值时网关返回 503，客户端退避重试

**C3. 双通道支持（HTTP + WebSocket 发送）**

把 `NEW_MESSAGE` 接口同时挂在 WebSocket 上：
- 长连用户走 WS（节省握手）
- 短连场景仍走 HTTP（兼容旧客户端 / 代理穿透）

**C4. 多机房灾备**

- MQ 跨机房 mirroring（RabbitMQ Federation 或换 Kafka MirrorMaker）
- ChatSession / Message 服务在两机房部署，etcd 跨机房注册带机房标签
- Transmite 优先就近 RPC，失败再跨机房

**C5. 冷热数据分离**

- `message` 表按月 partition，3 个月以上归档到冷库（TiDB / OSS Parquet）
- `user_timeline` 仅保留 30 天，更早的删除（用户可主动从 message 表查）
- ES 索引按月 alias，过期 close

---

## 3. 关键节点的"应该长什么样"

下面是改造完成后，发送一条群消息的目标链路：

```
客户端
  │ WS 发送 NewMsgReq{client_msg_id, ssid, content}
  ▼
Gateway（无状态）
  │ 鉴权 → 限流 → brpc 转 Transmite
  ▼
Transmite
  ① select_by_client_msg → 命中即返回旧 message
  ② 并行 RPC（user info + 群成员，群成员走 Redis 缓存）
  ③ 申请 message_id（雪花，自动 worker_id）
  ④ 申请 session_seq + 每个成员 user_seq（Redis INCR）
  ⑤ 写 InternalMessage 到 MQ；publish_confirm
  ⑥ 写自己（发送者）的 timeline（保证发送者 UI 一致）
  ⑦ 返回 success + message_id + session_seq
  ▼
MQ
  ├─ db_queue → DB Consumer
  │    若 member_count > 200：分片到 fanout.{0..15}
  │    否则：直接事务写 message + N timeline
  │    完成后 → 投递 push_queue
  │
  ├─ es_queue → ES Consumer（仅文本）
  │
  └─ push_queue → Push Service
       lookup 在线设备 → WS 下发 → 等 ack → 失败入待重传

客户端
  - 收到 ack 后 update local last_user_seq
  - 心跳带 last_user_seq；server 比对发现缺漏自动补送
  - 下次 App 启动调 GetOfflineMsg(last_user_seq) 把空洞补齐
```

---

## 4. 落地优先级建议

| 阶段 | 工期 | 依赖 |
|---|---|---|
| A1 + A2（seq + 客户端去重） | 3 天 | PR #5 / #6 / #7 已就位 |
| A3（送达语义）              | 2 天 | A1 |
| A4（worker_id 自动分配）    | 1 天 | 独立 |
| B4（文件上传前置）          | 1 天 | 客户端配合 |
| B1（群成员缓存）            | 2 天 | 独立 |
| B2（大群分片）              | 5 天 | A1 |
| B3（推送服务独立）          | 7 天 | B2 |
| B5（推送 ACK + 重传）       | 3 天 | B3 |
| C1（死信失败转储）          | 2 天 | 独立 |
| C2（限流热点保护）          | 2 天 | 独立 |

A 阶段全部完成后已经可以扛单机房 5w QPS（千人群）；B 阶段完成可扛 50w QPS；C 阶段是多机房灾备。

---

## 5. 不在本文范围（但需配套）

- **客户端协议升级**：`client_msg_id` / `last_user_seq` 必须客户端配合
- **运维监控**：QPS / MQ 堆积 / 消费延迟 / 推送 ACK 率 等核心指标
- **压测**：每完成一个阶段都应有 baseline 测试，避免改回了别的洞

---

## 6. 总结

| 现状 | 目标 |
|---|---|
| MQ ack 当作发送成功 | timeline 写入 + 推送尝试才算成功 |
| 全局 message_id，会话内不连续 | (session_id, seq_id) 单调递增 |
| 重发 = 重复消息 | client_msg_id 幂等 |
| 大群单事务千行写 | 分片扩散，单事务 ≤ 200 行 |
| Gateway 直推，单机内存表 | 推送服务独立，Gateway 无状态 |
| 推送丢失靠下次拉补 | 推送 ACK + 待重传队列 + 心跳补送 |
| 消费失败无限重投 | DLX + 失败转储 |
| 文件上传在事务里 | 文件上传前置，事务无 RPC |
| 无限流 | 用户级 + 会话级 + 全局熔断 |

整改完成后，正确性问题（丢消息 / 重复消息 / 顺序错乱）全部消除，单机房支持百万级 DAU 群聊场景。
