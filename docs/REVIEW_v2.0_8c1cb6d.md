# Code Review Report — commit `8c1cb6d`

> **Subject**: feat: 拆分 Push 服务 + 消息链路加固（A/B/C 阶段）
> **Reviewer**: cpp-reviewer agent (independent pass)
> **Date**: 2026-05-13
> **Branch reviewed**: `docs/message-pipeline-review` → merged into `main`
> **Scope**: 35 files, +2145 / -334 lines
> **Verdict**: **🔴 BLOCK — 不可上线**

---

## 总体评估

架构方向正确，已知风险面（done 泄漏 / 续期租约误覆盖 / 跨实例同步阻塞）已修。但本次拆分仍引入若干 commit message **未提到的新问题**。其中 4 项 BLOCKER 任意一项都能让消息收发链路死掉。

修复优先级建议：

```
B1 - B4   →  让链路本身能跑通（必须）
M1 - M5   →  推送可靠性 / shutdown 安全（应该）
M6 - M10  →  防御性兜底（可灰度）
MINOR/NIT →  cleanup 类，可与 v2.1 合并
```

---

## 1. BLOCKER（必须改）

### B1. ACK 链路在协议层断了：客户端无法生成合法 MSG_PUSH_ACK

- **现象**：
  - `NotifyMsgPushAck.user_seq` 是 ACK 所依赖的关键字段（`proto/notify.proto:31`）
  - 但下行 `CHAT_MESSAGE_NOTIFY` 携带的 `MessageInfo`（`proto/base.proto:131-145`）只包含 `seq_id`（会话级），**没有任何 `user_seq` 字段**
- **影响**：
  - 服务端 `PushServiceImpl::PushToUser` 把 `user_seq` 写进 `UnackedPush`（`push_server.h:64-68`），却从不告诉客户端 `user_seq` 是多少
  - → 客户端没法正确填 ACK
  - → `UnackedPush` 永远不会被 zrem
  - → ZSET 无限增长 + ACK 收敛 RPC（`UpdateAckSeq`）永远拿不到正确 seq
- **修法**：
  - 选项 A：在 `NotifyNewMessage` 加 `optional uint64 user_seq`，由 push 服务下发时按收件人填
  - 选项 B：ACK 改用 `(chat_session_id, seq_id)` 而非 user_seq

### B2. Transmite 与 Message 的 MQ 交换机/队列名不一致 → 消息根本投递不到 message 服务

- **现象**：
  - `transmite_server.conf:29-31`：`mq_msg_exchange=msg_exchange` / `mq_msg_queue=msg_queue`
  - `message_server.conf:23-26`：`mq_msg_exchange=chat_msg_exchange` / `mq_msg_queue_db=msg_queue_db` / `mq_msg_queue_es=msg_queue_es`
  - 两边交换机名都不一样，绑定也不一样。FANOUT 也救不回来。
- **影响**：
  - Transmite 投到 `msg_exchange`，message 在 `chat_msg_exchange` 上消费
  - **整条 DB / ES 写入链路从这次配置改动起完全断开**
  - 看起来是上一轮 ES 改名时只动了一边
- **修法**：统一两边为同名 exchange + 正确 binding；并在启动时做配置交叉校验。

### B3. 9001 端口在两个进程同时监听

- **现象**：
  - `gateway_server.conf:5`：`websocket_listen_port=9001`，gateway 仍然 `listen(9001)`（`gateway_server.h:122`，本次 diff 没有删 WS 服务器）
  - `push_server.conf:9`：`ws_port=9001`，push 也 listen 9001
  - `docker-compose.yml:120`：gateway 容器只暴露 9000；`docker-compose.yml:138`：push 暴露 9001
- **影响**：
  - 容器内 gateway 还会 `listen(9001)`、ws 路由表也还在 gateway（`onMessage` handler 走 `_connections->client/insert`）
  - 多 gateway 实例时 9001 被另一个进程占用会启动失败
  - 单容器场景虽然不冲突但 gateway 仍然会接受 WS 连接，把客户端鉴权/心跳/ACK 都黑洞掉（gateway 不再处理这些 NotifyType）
- **修法**：从 gateway 中彻底移除 WS 服务器（`onOpen/onClose/onMessage`、`_connections`、`websocket_listen_port` 配置项、`make_server_object` 的 ws_port 参数、`_ws_server.run()`），或至少把 `websocket_listen_port` 默认 0 / 不再 listen。

### B4. `UpdateAckSeq` 静默成功掩盖错误

- **现象**：
  - `message_server.h:544-548`：
    ```cpp
    if (!_mysql_member_table) { response->set_success(true); return; }
    ```
    `_mysql_member_table` 永远在构造函数里 `make_shared` 出来（`message_server.h:52`），不可能是 null
  - 这段降级代码永远走不到，但更糟的是注释说"未注入则降级"暗示作者并没有验证写入路径
  - Push 收 ACK 后也是 `brpc::DoNothing` fire-and-forget（`push_server.h:199`），ACK 写库失败完全无声
- **附加风险**：
  - `UpdateAckSeq` 写的是 `chat_session_member.last_ack_seq`，但 `message_server` 没有声明 chatsession DAO 是这张表的写权限边界
  - 两个服务并发改同一张表（chatsession 服务也持有 `ChatSessionMemberTable`），没有 `update_last_ack_seq` 单调性保证
  - DAO 实现里需是 `WHERE new_seq > last_ack_seq`（请验证 `mysql_chat_session_member.hpp:294`）

---

## 2. MAJOR（上线前应改）

### M1. push 服务 shutdown 顺序仍有 UAF

- `push_server.h:266-268`：brpc `RunUntilAskedToQuit()` 返回后才 `_ws_server->stop()`
- 但 `_push_subscriber` **永不显式停止**；其 MQ 消费线程持续调用 `_push_service->onPushMessage`，而 `_push_service` 由 brpc `SERVER_OWNS_SERVICE` 持有，brpc::Server 析构会 delete 它
- **MQ 消费线程访问已释放对象 → UAF**
- **修法**：start() 退出顺序应是 (1) 主动 `_push_subscriber->stop()` → join；(2) `_ws_server->stop()` + join；(3) brpc Stop + Join

### M2. WS 同一连接的并发 send 没有 per-conn 序列化

- `push_server.h:217-231` `_local_send` 同时被三处调用：
  - MQ 消费线程（onPushMessage）
  - brpc IO 线程（PushToUser / PushBatch）
- websocketpp 的 `connection::send` 不是线程安全的（除非 asio_no_tls 在 strand 内）
- 多线程并发 send 同一 conn 会出现帧错乱 / crash
- **修法**：要么对每个 conn 加 strand/mutex 排队，要么把 send 转发到 ws io_service 的 strand

### M3. PushOutbox 没有任何 reaper 代码

- `data_redis.hpp:500-525` 有 enqueue/peek/remove；`message_server.h:664/669` 在写失败时入队
- 但**整个 commit 没有任何地方启动 reaper 线程**去消费这个 ZSET
- 失败的推送会永远积压在 Redis 直到 OOM
- 注释虽然说"由后台 reaper 重投"，但代码根本不存在
- **修法**：补一个 reaper（建议放在 message 服务）+ 单实例锁（SETNX）避免多副本并发重投

### M4. worker_id 租约失效后无人响应

- `worker_id.hpp:84-85` 暴露 `lease_lost()`，注释说"业务层据此优雅退出"
- 但 `transmite_server.h` 内**没有任何线程在轮询 lease_lost**
- Snowflake 续期失败后 `_id_generator` 还在用同一个 worker_id（已被别人占据）继续发号 → **雪花重号**
- 整段防御逻辑形同虚设
- **修法**：在 transmite builder 启动一个 watchdog 线程，轮询 `_worker_allocator->lease_lost()`，检测到就 `_rpc_server->Stop()` + 主动 unregister + abort

### M5. UnackedPush.drain 只读不删 + 心跳重发未实现 + 50 上限硬编码

- `push_server.h:204-211`：心跳触发 drain 拿到一批 user_seq，然后**只 LOG_DEBUG 不实际重发**
- 这说明本次重传链路只搭了壳子，实际未联通
- `drain` 不做 zrem → 高峰期 5000 条 unacked 时每次心跳只拿 50 老的，内存/网络会反复扫；并且如果 ack 晚于 drain，就会在再次心跳时把同一条重新"打算重发"
- `drain` 的 `max_age_sec=5` 和 `kUnackedTtl=7d`：未 ack 项 7 天后整 key TTL 到期消失，消息彻底丢；调用方却拿不到任何信号
- **修法**：drain 改 ZRANGEBYSCORE+ZREM 语义；或者由 ack 路径回写一个高水位 + drain 只读 score < 高水位 的项；上限改成可配置；并且把"实际重发"补完才能上线

### M6. Push 跨实例 PushBatch 没有失败回退到 unacked outbox

- `push_server.h:172-179`：跨实例 PushBatch 用 `brpc::NewCallback` 异步，失败时只 LOG_WARN，不 enqueue 到 PushOutbox 也不在 unacked 上提升 score 推迟重试
- 如果对端 push 实例宕机，本端早已写过 unacked（`push_server.h:121-123`），但 unacked 里只有 user_seq，没有 message 内容
- `drain` 出来后没法重组 NotifyMessage（注释里也承认了 `push_server.h:204-205`）
- **这条路实际上不闭合**
- **修法**：unacked 应当存 `(user_seq, payload-or-mid)`；或者重发路径走查 message 表回查后再发

### M7. Transmite 限流早于 RPC，user_id 任意输入即可引流

- `transmite_server.h:124-135`：限流键直接用请求里的 `uid`/`chat_ssid`，而幂等查询和限流都没有先做"会话成员校验"
- 攻击者伪造 `uid=victim` + `chat_ssid=任意` 就能把受害人的 RateLimiter 计数器打爆，让真实用户被 reject
- 同时固定窗口 60s/600 这个数和窗口长度没暴露成 conf flag，无法运行期调参
- **修法**：限流前至少先核对 `(uid, chat_ssid)` 是否在 Members 中（这本来已经查过了，调换顺序即可）；阈值/窗口改 -DEFINE_int32

### M8. Members 缓存与会话写路径有竞态：发送时正好成员变动 → 推送漏人

- ChatSession 在 `Add/Remove/Quit/Create` 都 `invalidate(ssid)`（`chatsession_server.h` 五处）
- 但 transmite 的流程是「先读 cache 拿到 list → 计算 user_seq → 落 MQ」
- 在 read-and-compute 之间另一个事务把成员加入了，**新成员会拿不到 user_seq 也不在 member_id_list 里** → 这个新成员永远收不到这条消息
- 写扩散无 timeline 行；读扩散即使能读到主表，他的 user_seq 也对不上、ACK 不上
- **修法**：成员变更应通过 chatsession 的 RPC 拿 version；或 invalidate 后 transmite 走"严重 fallback：读到空就 RPC 回填"。当前只能容忍 30 分钟的过期，但增量竞态没解决

### M9. 大群读扩散切换缺迁移期兼容

- `transmite_server.h:23` `LARGE_GROUP_THRESHOLD = 200` 是写时判定
- `message_server.h:184-195` `GetRecentMsg` 在 timeline 为空时才 fallback `recent_by_seq`
- **场景 1（群增长跨 200）**：老消息走的是 timeline，新消息只写 message 主表 → timeline 还有老消息（非空）→ 永远走 timeline 路径 → **读不到新消息**
- **场景 2（群 shrink 到 < 200）**：老消息只在主表，新消息写 timeline，时间窗里 GetRecentMsg 只看 timeline 会丢老消息
- **修法**：要么 GetRecentMsg 永远 union 两条路径取并集；要么阈值变化时打迁移 flag，老群保留写扩散

### M10. fallback worker_id 与已分配的 slot 可能撞号

- `worker_id.hpp:80-81` Redis 连不通时直接 `_allocated.store(fallback_worker_id)` 并返回 fallback
- 其它能连 Redis 的实例已经把 fallback_id 这个 slot 占了，本进程发出去的雪花 ID 就重号
- **修法**：fallback 用本机 hostname 哈希 + 一个高位偏置，使其落到正常 incr 轮转之外的区段；或者 Redis 不通时直接 abort 拒绝启动

---

## 3. MINOR（建议）

### N1. message 服务的 chatsession_service_name 未声明 channel

- `message_server.h:918-920` 只 declared file/user，但构造 impl 时把 chatsession 传了进去（参数 `_chatsession_service_name`，目前未被任何 RPC 调用使用）
- 如果未来真用了 → channel 拿不到

### N2. message_server.cc 缺 chatsession_service flag

- `message_server.conf` 没有 `-chatsession_service`，依赖默认值
- 建议显式写入避免新环境踩坑

### N3. NewMessageReq.session_id 与 chat_session_id 共存

- `transmite.proto:18-22`，注释和职责不清
- 请要么删除 `session_id` 要么文档化

### N4. Connection.reap 在长心跳间隔下会误清正常连接

- `connection.hpp:107` `now - last_active_ts > ttl_sec`
- 客户端心跳间隔若为 30s、ttl 设短（如 60s），网络抖动一次 ws 包没到就被 reap，断开后又立刻重连
- 建议 ttl 默认至少 3×心跳；reap 调用方还要同步 `OnlineRoute::unbind`
- 当前 reap 返回 `(uid, conn)` 让调用方处理，但**没有任何代码调用 reap**（搜索全仓只在头文件里定义）。**死代码**

### N5. RateLimiter::allow 失败默认放行

- `data_redis.hpp:482-484`：限流器 Redis 抖动就完全失效，防 DDoS 时反而是攻击窗口
- 建议 Redis 故障时按"严格模式"配置：默认拒绝，或至少加本地令牌桶 fallback

### N6. NotifyMsgPushAck.message_id 类型不一致

- `proto/notify.proto:30`: `string`
- `proto/notify.proto:30`: `int64`
- 同一字段编号、不同类型，protobuf wire format 解析会出错
- int64 是 varint，string 是 length-delimited，wire type 完全不同 → **反序列化失败**

### N7. OnlineRoute::bind 用 SET + EXPIRE 两步，原子性差

- `data_redis.hpp:425-427` 中间崩了会留无 TTL 的 key
- 建议 SADD 后用 pipeline，或干脆改为 `SET ... EX`

### N8. 新协议字段仅在 server proto2/proto3 之间向前兼容

- `MessageInfo.seq_id/client_msg_id` 是非 optional 的 proto3 默认字段
- 旧客户端反序列化时这些字段为 0 / 空 → 旧客户端把 seq_id=0 的新消息当"无效消息"丢弃
- 建议 release notes 强制要求客户端最低版本

### N9. NotifyType 数值跳号 0..4 + 49..51

- 枚举设计上没问题，但建议把客户端→服务端的 49+ 区间显式注释成"upstream only"
- 让 push 服务在收到 server-only NotifyType 时直接关闭连接，避免被构造的 fake 包扰乱

---

## 4. NIT（可选）

| ID | 文件:行 | 描述 |
|---|---|---|
| NIT-1 | `push_server.h:158-160` | `auto *req = new PushBatchReq(); ...` 三处 raw new，建议 `unique_ptr` + `release()` 给 brpc |
| NIT-2 | `data_redis.hpp:60` | `kPushOutbox` 全局 Sorted Set 但没有按 hash 分片，单 key 在 outbox 高峰可能过 10MB；建议按时间分桶 |
| NIT-3 | `worker_id.hpp:60-61` | `incr % 1024 + 1024) % 1024` —— `incr` 总是返回正数，不需要 `+ kMaxWorkerId`；可读性不佳 |
| NIT-4 | `worker_id.hpp:155` | `_renew_thread` 在析构里 join，但 stop() 之外没有处理 `_c` 已经 drop 的情况；如果上层先释放 redis client，stop 里还要 `_c->get/del` 会走到僵尸客户端 |
| NIT-5 | Transmite Builder | `make_id_generator_object` 把 `WorkerIdAllocator` 错误捕获后**仍然 fallback** —— 这跟 M4 串起来后果是双重沉默 |
| NIT-6 | `message_server.h:701` | ES 路径只过滤 STRING 消息，但写 ES 没有 `seq_id/client_msg_id`；后续 D 阶段建议补 |
| NIT-7 | `data_redis.hpp:46` | `kPushOutbox` 没带末尾冒号，一致性上跟其它前缀不同，提醒注意 key 拼接 |

---

## 5. 已确认修复（不重复抱怨）

| 项 | 验证位置 |
|---|---|
| ✅ Transmite publish_confirm done 双保险 | `transmite_server.h:255-282`，try/catch + atomic exchange |
| ✅ 跨实例 PushBatch 已改异步 `brpc::NewCallback` | `push_server.h:172-179` |
| ✅ worker_id 续期不再裸 SET 覆盖 | `worker_id.hpp:128-139` —— 设计正确，但 lease_lost 信号无人消费见 M4 |
| ✅ `UnackedPush::drain` 用 `BoundedInterval` 不再扫错区间 | `data_redis.hpp:566-570` |
| ✅ onDBMessage / onESMessage redelivered 二次失败转 NackDiscard | `message_server.h:681-683`、`:723-727` |
| ✅ chatsession 5 处成员变更全部 `invalidate` | `chatsession_server.h:224、441、488、890` |
| ✅ FriendAdd 推送目标已改成 `respondent_id` | `gateway_server.h:720` |

---

## 6. 涉及文件

```
proto/base.proto
proto/notify.proto
conf/transmite_server.conf
conf/message_server.conf
conf/gateway_server.conf
conf/push_server.conf
gateway/source/gateway_server.h
push/source/push_server.h
push/source/connection.hpp
transmite/source/transmite_server.h
message/source/message_server.h
common/worker_id.hpp
common/data_redis.hpp
proto/notify.proto
```

---

## 7. 修复执行建议

### 7.1 Sprint 1（必须，1-2 天）

- [ ] **B2**：统一 Transmite/Message 的 MQ exchange/queue 命名，启动时交叉校验
- [ ] **B3**：从 gateway 删除 WS 服务器；docker-compose 端口对齐
- [ ] **B1**：`MessageInfo` 加 `user_seq`，由 push 按收件人填充
- [ ] **B4**：`UpdateAckSeq` 走真正错误返回 + DAO 单调性 `WHERE new_seq > last_ack_seq`

### 7.2 Sprint 2（应该，3-5 天）

- [ ] **M1**：push 服务 shutdown 显式 stop subscriber
- [ ] **M2**：per-conn strand/mutex 序列化 ws send
- [ ] **M3**：实现 PushOutbox reaper + 单实例锁
- [ ] **M4**：transmite watchdog 轮询 lease_lost
- [ ] **M5**：UnackedPush 真正重发 + zrem 语义修正

### 7.3 Sprint 3（防御，1 周）

- [ ] **M6**：跨实例 PushBatch 失败入 outbox / unacked 带 payload
- [ ] **M7**：限流前先 Members 校验；阈值改 conf flag
- [ ] **M8**：Members 缓存竞态 — chatsession 加 version
- [ ] **M9**：GetRecentMsg union 两条路径
- [ ] **M10**：fallback worker_id 高位偏置

### 7.4 Sprint 4（cleanup）

- 全部 MINOR + NIT 一次性扫掉
- 并发回归测试 + 容量压测

---

**结论再强调**：核心阻塞 3 项（B1/B2/B3）任意一项都能让消息收发链路死掉。建议先 fix B1-B4，再清掉 M1-M5，才有上线讨论价值。

> 评审依据：commit `8c1cb6d` 全量 diff + 配置文件 + proto 文件交叉对比；不扩展到更早 commit。
