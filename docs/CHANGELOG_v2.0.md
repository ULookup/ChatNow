# ChatNow v2.0 变更总览

> 周期：2026-05-12 ~ 2026-05-13
> 范围：6 个 commit，共 79 文件变化，**+7449 / -2524** 行
> 主题：从「单 Gateway 直推 + 单事务写扩散」演进到「Ingest / Store / Push 三角架构 + 增量同步 + 高可用兜底」
>
> 本文档以「上线视角」串联 6 个 commit，为 v2.0 提供单一可读的变更说明。
> 配套设计文档：`docs/MESSAGE_PIPELINE.md`（蓝图）、`docs/CHANGELOG_v2.0_review.md`（review 结论，待补）。

---

## 0. Commit 索引

| # | Hash | 日期 | 标题 | 规模 |
|---|---|---|---|---|
| 1 | `8444ac1` | 2026-05-12 20:45 | refactor: 重构 ODB 表结构以支撑高并发 IM 场景 | 20 文件 / +1813 -527 |
| 2 | `2ad418a` | 2026-05-12 22:30 | feat(common): 重构 mysql_*.hpp DAO + 新增 4 张表 DAO | 11 文件 / +1725 -859 |
| 3 | `bff64ce` | 2026-05-12 22:48 | feat(common): 完善基础设施封装 | 9 文件 / +1024 -448 |
| 4 | `faf4dff` | 2026-05-12 22:56 | feat(common): 完善 ES 客户端 / 业务 ES 索引 / Snowflake 注释 | 3 文件 / +403 -356 |
| 5 | `72b01a1` | 2026-05-12 23:06 | docs: 消息链路缺陷分析与高并发/高可用改造蓝图 | 1 文件 / +339 |
| 6 | `8c1cb6d` | 2026-05-13 02:40 | feat: 拆分 Push 服务 + 消息链路加固（A/B/C 阶段） | 35 文件 / +2145 -334 |

线性堆叠关系（从老到新）：

```
main(0c51f2d)
 └─ 8444ac1  schema 重构（ODB 层）
     └─ 2ad418a  DAO 重写 + 4 张新表
         └─ bff64ce  基础设施封装（logger/redis/utils/etcd/channel/mysql/mail/mq/asr）
             └─ faf4dff  ES 客户端 / 索引完善
                 └─ 72b01a1  问题盘点 + 改造蓝图（设计文档）
                     └─ 8c1cb6d  Push 拆分 + 消息链路 ABC 阶段落地
```

---

## 1. `8444ac1` — ODB Schema 重构（地基）

**目标**：把数据模型从「IM 玩具版」升级到「能扛高并发 + 增量同步」。

### 1.1 核心改造

| 表 | 关键变化 |
|---|---|
| `message` | 引入 **`(session_id, seq_id)` 唯一键** —— 会话级增量同步基石；新增 `client_msg_id` 客户端断网重发幂等去重；修正 `file_size` 类型 bug（`unsigned char` → `bigint unsigned`） |
| `user_timeline` | 新增 **`user_seq` 用户级游标** —— 单游标即可拉跨会话增量 |
| `chat_session` | **移除 `last_message_*` 行级写热点字段**，迁到 Redis 缓存；单聊新增 `peer_user_id` 直接落主表，零 self-join |
| `relation` | 改 **双向冗余存储**，支持单方拉黑 / 删除 / 备注 / 分组 / 星标 |
| `chat_session_member` | 新增 `alias / inviter_id / join_source / mute_until / is_quit` |
| `friend_apply` | 新增 `source / greeting / remark`，便于风控与体验 |

### 1.2 新增表

| 表 | 用途 |
|---|---|
| `user_device` | 多端登录、推送 token、在线状态、风控审计 |
| `message_read` | 群消息已读回执（按需开启） |
| `message_attachment` | 多附件场景（多图 / 视频 / 缩略图） |
| `message_mention` | @ 提及拆表，让"@我"通知走索引 |

### 1.3 索引优化

- 删除所有低基数枚举字段的单列冗余索引
- `(session_id, seq_id)` 改 **UNIQUE** —— 兜底防 MQ 重投 / 双写
- `(user_id, device_id)` 改 UNIQUE —— 防设备记录堆积
- `push_token` 加 UNIQUE —— 防账号切换残留

### 1.4 字段补全

- 各主表统一补 `update_time`，便于客户端增量同步
- 新增 `README.md` 项目总览
- 删除旧 `sql/*.sql`（改由 ODB `generate-schema` 重出）

### 1.5 文档化

- 每个 ODB 文件加上 **「设计定位 + 字段速览 + 索引策略」四段式注释**

---

## 2. `2ad418a` — DAO 重写 + 4 张新表 DAO

**目标**：让 DAO 层匹配新 schema，并把「事务感知 / 单调推进 / 防注入」三个原则落到所有写路径。

### 2.1 7 个旧 DAO 重写

| DAO | 关键变化 |
|---|---|
| `mysql_user` | 新增 phone 登录 / 状态机 / 登录信息回写；insert/update 自动维护 `update_time`；批量查询用 `in_range` 防注入 |
| `mysql_relation` | 双向关系软删除（DELETED 而非 erase）；新增备注/分组/星标/拉黑；`friends()` 仅返回 NORMAL 对端 |
| `mysql_friend_apply` | 删除物理 remove；`update_status` 状态机入口、`exists_pending` 判重、`select_pending` 走 `idx_peer_status_time` |
| `mysql_chat_session` | 移除 `last_message_*`；`select_single_by_peer` 基于 `peer_user_id` 直查；`dismiss` 软删除；`bump_max_seq` 单向递增 |
| `mysql_chat_session_member` | 退群 `set_quit` 软删 + 二次入群 `rejoin` 复用旧行；已读/送达游标改 seq 维度且单调递增；`list_ordered_by_user` 不再依赖 `cs.last_message_time` |
| `mysql_message` | 查询走 `(session_id, seq_id)` 唯一索引；新增 `select_by_client_msg / recent_by_seq / range_by_seq / max_seq_of_session / mark_revoked / mark_deleted` |
| `mysql_user_timeline` | 切到 `user_seq`；新增 `list_session_after/before/latest`；`mark_delivered/mark_read` 单调推进；`is_read` 合入 `TimelineDeliverStatus` |

### 2.2 4 个新 DAO

| DAO | 用途 |
|---|---|
| `mysql_user_device` | 多端登录 upsert / 心跳 / 在线设备查询 / 强制下线 / push_token 全局迁移 |
| `mysql_message_read` | 群消息已读回执（单 ack + 批量 ack） |
| `mysql_message_attachment` | 多附件场景批量 insert + 单/批 list；事务感知 |
| `mysql_message_mention` | @ 提及独立成行；`user_id="*"` 表示 @全体；`list_my_mentions` 走 `idx_user_msg` |

### 2.3 通用约定

- 所有 `mark_* / set_*` 仅在状态向前推进时写，避免回退覆盖
- **写事务感知**：`message / timeline / attachment / mention` DAO 都支持挂载到外部事务，便于消息存储服务"消息+timeline+附件"原子落库
- 批量查询全部改 `odb::query::xxx.in_range`，移除字符串拼接

---

## 3. `bff64ce` — 基础设施封装

**目标**：补齐 IM 高并发 / 高可用所需的基础组件，并修若干隐藏 bug。

### 3.1 模块差量

| 模块 | 关键变化 |
|---|---|
| `logger.hpp` | `g_default_logger` 改 inline 防多 TU 重定义；发布模式启用 async + rotating file（50MB×10）；warn 以上即 flush，其它 3s 批量；LOG_xxx 抽 basename |
| `data_redis.hpp` | 工厂支持连接池（`pool_size / wait_timeout / socket_timeout`）；key 前缀 namespace（`im:sess:` / `im:status:` / `im:seq:`）；Session/Status/Codes 全部带 TTL；新增 **SeqGen / LastMessage / DeviceSet / ReadAck** |
| `utils.hpp` | 修复 `uuid()` 局部 atomic 计数器 bug；新增 `verify_code(n)` / `single_session_id(a,b)` / ptime ↔ ms 时间戳工具；全部 inline |
| `etcd.hpp` | 修复 `~Registry` 错调 `Lease()`；lease 30s（旧 3s 太短）；新增 `unregister()` 优雅退出；统一 LOG_xxx |
| `channel.hpp` | `_index` 改 atomic 防 RR 竞争；超时改有限值（1s 连接 / 3s RPC）；`size()` 暴露节点数 |
| `mysql.hpp` | 默认 charset `utf8mb4`（旧 `utf8` 存不下 emoji）；新增 `ping()` / `TxnGuard` 事务感知 RAII |
| `mail.hpp` | 修多处错误返回路径漏 cleanup；引入 RAII（`CurlHandle / SListHandle`）；新增 connect/total timeout |
| `rabbitmq.hpp` | **修复 DLX 命名 bug**（`exchange + dlx_` → `dlx_ + exchange`）；`delayed_ttl` 单位明确为毫秒；库内不再 abort()；`consume()` 加 qos prefetch（默认 64）；优雅关闭顺序 |
| `asr.hpp` | 抽出 `SpeechRecognizer` 接口；空结果防御 + 异常捕获 |

---

## 4. `faf4dff` — ES 客户端 / 索引完善

### 4.1 `icsearch.hpp`

- `Serialize/UnSerialize` 加 inline
- 移除调试 `std::cout`
- 新增 `ESUpdate`：upsert 部分字段，IM 资料修改路径常用
- `ESSearch`：
  - **修隐性 bug**：must + should 共存时强制 `minimum_should_match=1`，避免 should 仅作打分加权不参与命中
  - 新增 `page(from,size) / sort_by`
  - 错误日志脱敏（body 仅 trace 输出）

### 4.2 `data_es.hpp`

- `ESUser` 补 `phone / status`；`search()` 同时支持 mail / phone / user_id / nickname
- `ESMessage` 补 `seq_id / status`；仅 `status=NORMAL` 的文本消息进入索引；`search()` 时间倒序 + 分页
- `ESChatSession` 补 `update_time`，按变更时间倒序
- 命名统一 `create_index`（保留 `createIndex` 兼容）

### 4.3 `snowflake.hpp`

- 注释完善，逻辑不变；`WaitUntil` 1ms 起步策略保留

---

## 5. `72b01a1` — 问题盘点 + 改造蓝图

**产出**：`docs/MESSAGE_PIPELINE.md`，对照源码定位 **16 个真实问题**。

### 5.1 P0（正确性致命）

1. MQ ack 被当作发送成功（实际只能保证 broker 收到）
2. 写库与推送两条独立路径，存在"收到推送但拉不到历史"窗口
3. 缺 `(session_id, seq_id)` 单调序号 → 客户端无法增量同步 / 算未读
4. 缺 `client_msg_id` 幂等去重 → 断网重发会重复
5. Snowflake worker_id 无自动分配 → 多实例撞号
6. 大群消息单事务千行写 + 文件上传 → 慢消费连锁阻塞

### 5.2 P1（架构问题）

7. 群成员每条消息都查一次 ChatSession，无缓存
8. Gateway 直推 + 内存连接表 → 无法水平扩
9. WS 推送无 ack，丢消息靠下次拉补
10. DB 事务里做远程文件上传 → 连接池恶化

### 5.3 P2（性能 / 运维）

11. 没有 DLX 失败转储，永久失败消息会无限重投
12. 文件上传失败 NackRequeue → 重复上传产生孤儿文件
13. HTTP 发送 / WS 推送通道不对称
14. 无限流 / 热点保护 / 降级开关
15. 离线消息只能向后增量

### 5.4 改造路线（A → B → C）

| 阶段 | 工期 | 内容 |
|---|---|---|
| A 正确性 | ~6 天 | seq_id + client_msg_id + worker_id 自动分配 |
| B 性能 | ~18 天 | 群成员缓存 + 大群分片扩散 + 推送服务独立 |
| C 高可用 | ~7 天 | DLX 失败转储 + 限流 + 多机房灾备 |

文末附「目标链路」完整 ASCII 时序图。

---

## 6. `8c1cb6d` — Push 拆分 + 消息链路加固（重头戏）

**目标**：按蓝图落地三角架构（**Ingest = Transmite / Store = Message / Push = 新服务**），把项目推到「可上线」水位。

### 6.1 阶段 A — 正确性

| 项 | 落地 |
|---|---|
| 幂等去重 | Transmite 接入 `client_msg_id` 去重 + SeqGen（session_seq + user_seq） |
| Worker ID | 新增 `worker_id.hpp`：Redis 租约 + atomic + condition_variable 中断；续期失败标记 `lease_lost` 防止裸 SET 覆盖造成雪花重号 |
| onDBMessage | 直接消费 `InternalMessage` 携带的 seq；唯一索引冲突幂等丢弃 |
| 新增 RPC | `SelectByClientMsg / UpdateAckSeq` |

### 6.2 阶段 B — 写扩散性能 + Push 独立

#### 6.2.1 Redis 缓存矩阵

`data_redis.hpp` 新增完整封装：

| 模块 | 用途 |
|---|---|
| `Members` | 会话成员列表缓存 |
| `OnlineRoute` | uid → 所在 push 实例的路由 |
| `RateLimiter` | 用户级 / 会话级固定窗口计数 |
| `UnackedPush` | 未 ACK 推送 ZSET 重传缓冲 |
| `PushOutbox` | 投递失败 outbox |

#### 6.2.2 大群读扩散

- ChatSession 在成员变更时主动失效 `Members` 缓存
- 大群（≥200）改读扩散：仅写 `message` 主表
- `GetRecentMsg` 增 `recent_by_seq` 兜底

#### 6.2.3 Push 独立服务

- 新建 `code/server/push/` 服务：WS 终结、订阅 `msg_push_queue`、跨实例 `PushBatch` 异步并发
- 文件类型消息要求客户端前置上传 file_id；`onDBMessage` 不再调 PutFile
- ACK 收敛 + UnackedPush 重传缓冲（按 score 过滤成熟项）

### 6.3 阶段 C — 高可用

| 项 | 落地 |
|---|---|
| DLX | `onDBMessage / onESMessage` redelivered 二次失败转 NackDiscard 入 DLX |
| 限流 | Transmite 入口接入用户级 / 会话级限流（固定窗口计数器） |

### 6.4 Gateway 改造

- `NewMessage` 不再本机推送（推送链路完全交给 Push 服务）
- 5 处 NotifyMessage 推送（FriendAdd / AddProcess×2 / Remove / CreateSession）统一改走 `PushService.PushToUser` 异步 brpc → Gateway 真正无状态
- **修复 FriendAdd 推送目标错误**（之前推给申请人自己，应推给 `respondent_id`）

### 6.5 可靠性兜底

| 项 | 实现 |
|---|---|
| publish_confirm | 同步异常 + 异步回调双保险，杜绝 brpc done 泄漏 |
| 投递失败 | message → push_queue 失败入 PushOutbox（Redis ZSET），后续 reaper 重投 |
| Push stop 顺序 | ws 异常退出主动通知 brpc Stop(0) |
| 僵尸连接 | `Connection.reap(ttl_sec)` 老化作为安全网 |

### 6.6 协议升级

| Proto | 变更 |
|---|---|
| `base.proto` | `MessageInfo` 增 `seq_id / client_msg_id`；`int64 message_id` 加注释（Snowflake 高位恒为 0） |
| `transmite.proto` | `NewMessageReq/Rsp` 增 `client_msg_id / message_id / seq_id` |
| `notify.proto` | 新增 `CLIENT_AUTH / MSG_PUSH_ACK / CLIENT_HEARTBEAT` 与 payload |
| `push.proto` | 新建 `PushService::PushToUser / PushBatch` |
| `message.proto` | 新增 `SelectByClientMsg / UpdateAckSeq` |
| 客户端 | `NetClient.sendMessage` 注入 `QUuid client_msg_id` |

### 6.7 部署

- `docker-compose.yml` 新增 `push_server`，9001 WS 端口从 gateway 迁出
- 顶层 `CMakeLists.txt` 加入 `push` 子目录
- 新增 `chatsession_server.conf / push_server.conf`

### 6.8 内嵌评审

> 本次包含按 cpp-reviewer 报告修复的 14 项 + 1 项小问题
> （编译错误 / done 泄漏 / 跨实例同步阻塞 / 续期租约误覆盖 / drain 区间错 等）

---

## 7. v2.0 端到端能力对照

| 能力 | v1 | v2 |
|---|---|---|
| 消息幂等 | ❌ 客户端断网重发即重复 | ✅ `client_msg_id` 唯一键去重 |
| 增量同步 | ❌ 仅时间戳，跨会话拉补难 | ✅ `(session_id, seq_id)` + `user_seq` 双游标 |
| 多实例发号 | ❌ Snowflake 撞 ID | ✅ Redis 租约 + atomic 自动分配 |
| 推送架构 | ❌ Gateway 直推 + 本机连接表 | ✅ Push 服务 + OnlineRoute 跨实例路由 |
| ACK 收敛 | ❌ 无 | ✅ MSG_PUSH_ACK + UnackedPush 重传 |
| 大群写扩散 | ❌ 单事务千行写 | ✅ ≥200 切读扩散，主表只写一行 |
| 群成员查询 | ❌ 每条消息查 DB | ✅ Redis Members 缓存 + 主动失效 |
| MQ DLX | ❌ 无，失败无限重投 | ✅ redelivered 二次失败 NackDiscard 入 DLX |
| 限流 | ❌ 无 | ✅ 用户级 + 会话级固定窗口 |
| Gateway 状态 | ❌ 维护 ws 连接 + 5 处直推 | ✅ 完全无状态，全部走 PushService |

---

## 8. 已知风险与遗留（待 v2.1）

> 详见 cpp-reviewer 对 `8c1cb6d` 的完整 review。这里仅列出阻塞上线的核心项。

### 8.1 BLOCKER

| ID | 描述 |
|---|---|
| B1 | ACK 链路协议层断 — `NotifyMsgPushAck.user_seq` 字段在下行 `MessageInfo` 中不存在 |
| B2 | Transmite/Message 的 MQ 交换机/队列名两边对不上，DB / ES 写入链路全断 |
| B3 | 9001 端口仍在 gateway / push 双方监听；gateway 未彻底删除 WS 服务器 |
| B4 | `UpdateAckSeq` 静默成功 + ACK 写库 fire-and-forget |

### 8.2 MAJOR

| ID | 描述 |
|---|---|
| M1 | Push 服务 shutdown 顺序 UAF（`_push_subscriber` 未显式 stop） |
| M2 | `_local_send` 多线程写同一 conn，缺 per-conn 序列化 |
| M3 | `PushOutbox` 仅有 enqueue，**无 reaper 实现** → Redis OOM 风险 |
| M4 | `worker_id.lease_lost()` 信号无人轮询 → Snowflake 续期失败仍发号 |
| M5 | `UnackedPush.drain` 只读不删 + 心跳重发未实现 |
| M6 | 跨实例 `PushBatch` 失败仅 LOG_WARN，不入 outbox / unacked |
| M7 | 限流早于成员校验，可被伪造 uid 打爆受害人计数器 |
| M8 | Members 缓存 read-and-compute 与成员变更存在竞态 |
| M9 | 大群读/写扩散切换阈值跨越期会丢消息（GetRecentMsg 未 union 两条路径） |
| M10 | Redis 不通时 fallback worker_id 与正常 incr slot 重叠 |

### 8.3 上线前修复优先级

```
B1 - B4   →  让链路本身能跑通（必须）
M1 - M5   →  推送可靠性 / shutdown 安全（应该）
M6 - M10  →  防御性兜底（可灰度）
```

---

## 9. 容器化与监控（运维视角）

### 9.1 容器化（已具备）

- 8 个业务服务全部有 dockerfile：`gateway / transmite / message / push / file / friend / user / speech`
- 中间件 `etcd / mysql / redis / es / rabbitmq` 一并编排
- 宿主机卷挂载持久化

**离生产还差**：
- 无 `healthcheck`，靠 `restart: always` 兜底
- secret 硬编码在 yml（需切 `.env` / secrets）
- 单机 compose，未提供 K8s manifest / Helm chart
- 无 `deploy.resources.limits`

### 9.2 监控（待补）

当前为 0。推荐路径（按投入排序）：

| 层 | 方案 | 工作量 |
|---|---|---|
| L1 | brpc built-in（`/status` `/vars` `/rpcz` `/health`）暴露 + 运维直连 | 1 天 |
| L2 | Prometheus + Grafana + 中间件 exporter | 3-5 天 |
| L3 | 业务 bvar：`message_publish_total / unacked_push_size / push_outbox_lag / worker_lease_lost / dlx_total / rate_limited_total` | 1 周 |
| L4 | OpenTelemetry C++ + Jaeger 全链路 | 1-2 周 |
| L5 | promtail/Filebeat → Loki/ES 日志聚合 | 2-3 天 |

**最低生产监控套件**：5 条核心告警
1. DLX 增量异常
2. push_outbox 积压 > 1000
3. worker `lease_lost` > 0
4. ACK 收敛 P99 > 30s
5. WS 在线连接突降 50%

---

## 10. 文档与索引

- `README.md` — 项目总览（`8444ac1` 新增）
- `docs/MESSAGE_PIPELINE.md` — 16 项缺陷 + ABC 阶段蓝图（`72b01a1` 新增）
- `docs/CHANGELOG_v2.0.md` — 本文档
- 每个 ODB `.hxx` — 「设计定位 + 字段速览 + 索引策略」四段式注释（`8444ac1`）

---

**版本归属**：本文档对应 `main` 分支 HEAD `8c1cb6d`，作为 v2.0 候选基线。
进一步上线工作请基于 `v2.0` 分支推进，BLOCKER / MAJOR 修复完成后再 cut release。
