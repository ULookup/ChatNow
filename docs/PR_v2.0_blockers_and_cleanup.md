# PR 报告: 修复 v2.0 BLOCKER / MAJOR 问题并清理过度设计

> **分支**: `fix/v2.0-blockers-and-cleanup`
> **基线**: `main` @ `8c70dc3`
> **HEAD**: `7c79dbe`
> **规模**: 20 文件 / +735 / -258
> **PR 链接**: https://github.com/ULookup/ChatNow/pull/new/fix/v2.0-blockers-and-cleanup

本 PR 处理 `docs/REVIEW_v2.0_8c1cb6d.md` 中评审报告列出的 4 项 BLOCKER + 5 项核心 MAJOR，并对修复过程引入的过度设计、以及发现的若干历史遗留 bug 一并清理。每一项都通过 `cpp-reviewer` 子代理独立 review，全部 PASS。

---

## 0. 概览

| 类别 | 数量 | 项目 |
|---|---|---|
| BLOCKER | 4 | B1 ACK 链路 / B2 MQ 命名 / B3 Gateway 去 WS / B4 ACK 收敛 |
| MAJOR | 5 | M1 Push 关停 UAF / M2 WS send 串行化 / M3 Outbox reaper / M4 lease_lost watchdog / M5 心跳真重发 |
| 过度设计清理 | 7 | FOR UPDATE 兜底 / affected==0 二次 SELECT / drain 别名 / 关停三步日志 / reaper 重入保护 / SelfDeleteRpcClosure 抽公共 / gateway _pushNotify UAF |
| 历史遗留 bug | 5 | _redis_status 死字段 / update({csm,newcsm}) 编译错 / update_last_read_msg 命名错 / remove(*csm) 不存在 / 4 处 LOG 占位符不匹配 |

---

## 1. BLOCKER 修复

### B1 — ACK 链路协议层补 `user_seq`

**问题**：下行 `CHAT_MESSAGE_NOTIFY` 携带的 `MessageInfo` 没有 `user_seq` 字段，客户端无法生成合法 `NotifyMsgPushAck.user_seq` → `UnackedPush` 永远不会被 zrem，ZSET 无限膨胀，ACK 收敛 RPC 拿不到正确 seq。

**修法 (选项 A)**：

- `proto/base.proto` —— `MessageInfo` 新增 `optional uint64 user_seq = 8`
- `proto/base.proto` —— 客户端镜像同步
- `proto/notify.proto` —— 顺手把 `NotifyMsgPushAck.message_id` 由 `string` 改回 `int64`，与服务端 wire type 对齐（N6 顺手修）
- `push/source/push_server.h`：
  - `onPushMessage` 引入 `notify_template`（不带 user_seq，给跨实例 PushBatch 转发用）+ `build_payload_for(uid)` lambda（per-uid 重新序列化时填 user_seq）
  - `PushToUser` / `PushBatch` 收到带 `user_seq` 的请求时，深拷贝 NotifyMessage 后 `set_user_seq` 再序列化下发
  - `onClientNotify` 收到 ACK 时新增防御：`user_seq==0` 或 `user_id/chat_session_id` 空直接丢弃，避免大群读扩散场景污染 last_ack_seq

**反复推敲点**：
1. cpp-reviewer 抓到 lambda 化导致 `req->mutable_notify()->CopyFrom(notify)` 引用了已收敛的局部变量 → 提取 `notify_template` 解决
2. per-uid 序列化在大群场景下确实是 N×Serialize 的代价，但写扩散群本就 ≤ 200，可以接受；大群读扩散无 `user_seq` 进 unacked，自然不走该路径

---

### B2 — Transmite ↔ Message MQ exchange 命名对齐

**问题**：transmite publish 到 `msg_exchange`，message 在 `chat_msg_exchange` 上消费。FANOUT 也救不回来，DB / ES 写入链路完全断开。

**修法**：

- `conf/transmite_server.conf` —— `mq_msg_exchange=chat_msg_exchange`，`mq_msg_queue` / `mq_msg_binding_key` 留空
- `transmite/source/transmite_server.h::make_mq_object`：
  - exchange 空 → abort
  - 非空 queue/binding → WARN 并强制清空（防 transmite 误声明孤儿队列）
  - publisher-only 模式：FANOUT 路由忽略 routing_key
- `message/source/message_server.h::make_mq_object`：exchange / queue 空都 abort，启动期 INFO 日志打配置
- `common/rabbitmq.hpp`：
  - `MQClient::declare()` 当 `settings.queue.empty()` 时只声明 exchange（新增 `_declare_exchange_only` 私有方法），避免孤儿队列
  - `~MQClient` 把 `close + ev_break` 串成同一个 task，避免 quit watcher 抢在 close 前跑导致 broker 看到 abrupt disconnect

**反复推敲点**：
1. cpp-reviewer 第一轮指出 WARN-only 后仍把 queue/binding 透传给 declare → 修为强制清空
2. 反复确认 FANOUT + binding key 关系：FANOUT 完全忽略 binding，仅看 exchange 名

---

### B3 — Gateway 彻底移除 WebSocket 服务器

**问题**：9001 端口在 gateway 与 push 两个进程同时监听；客户端鉴权/心跳/ACK 在 gateway 端被黑洞掉。

**修法**：

- `gateway/source/gateway_server.h`：
  - 移除 `connection.hpp` include；删 `_connections` / `_ws_server` / `onOpen` / `onClose` / `onMessage` / `keepAlive`
  - 构造函数去掉 `websocket_port`；只保留 HTTP 路由
  - `start()` 改为阻塞 `_http_server.listen(...)`；listen 返回 false 时 LOG_ERROR + abort（避免静默 exit 0）
  - 顺手删死字段 `_redis_status`
- `gateway/source/gateway_server.cc`：
  - 保留 `DEFINE_int32(websocket_listen_port, ...)` 作 deprecated 占位（兼容旧 conf；gflags 默认 fail-on-unknown 会让旧部署起不来）
  - main 检测到 != 0 时 LOG_WARN
- `conf/gateway_server.conf`：`-websocket_listen_port=0`
- `gateway/source/gateway_server.h::_pushNotify`（顺手修预存 UAF）：
  - 旧实现是栈上 `brpc::Controller cntl/req/rsp` + `brpc::DoNothing` 异步回调，回调访问已析构对象 → 改用 `SelfDeleteRpcClosure<PushToUserReq, PushToUserRsp>`

**反复推敲点**：
- cpp-reviewer 抓到 `_http_server.listen` 返回值被丢 → 加 abort 兜底

---

### B4 — `UpdateAckSeq` 真正错误返回 + DAO 单调性

**问题**：原代码 `if(!_mysql_member_table) { success=true; return; }` 是死代码（构造函数始终 make_shared），但暗示作者没验证写入路径；Push 端调用是 `brpc::DoNothing` fire-and-forget，写库失败完全无声。DAO 实现的 `if(m->last_ack_seq() < new_seq)` read-modify-write 在并发下不严格。

**修法**：

- `common/mysql_chat_session_member.hpp`：
  - 重构 `update_last_ack_seq` / `update_last_read_seq`，共用助手 `_atomic_advance_seq(column, ssid, uid, new_seq)`
  - 单条原子 SQL：`UPDATE chat_session_member SET <col> = GREATEST(<col>, n) WHERE session_id=? AND user_id=?`
  - DB 层强保证单调；不会被 chatsession 端的全行 UPDATE 覆盖回退
  - `column` 走代码内常量白名单，无注入面；`ssid/uid` 走 `_escape_id` 防御性转义
- `message/source/message_server.h::UpdateAckSeq`：
  - 删假降级 `if(!_mysql_member_table)`
  - 入参非法（user_id / chat_session_id 空 / user_seq=0）→ 直接返回 errmsg
- `push/source/push_server.h::onClientNotify` MSG_PUSH_ACK 分支：
  - 用 `SelfDeleteRpcClosure<UpdateAckSeqReq, UpdateAckSeqRsp>` 替代 `brpc::DoNothing`
  - 失败时 LOG_WARN 包含 uid + seq + 错误信息

**反复推敲点**：
1. cpp-reviewer 第一轮发现 `brpc::NewCallback` 不接受 capturing lambda → 抽 `SelfDeleteRpcClosure` 模板类
2. 第二轮指出 chatsession 全行 UPDATE 仍可能覆盖 last_ack_seq（lost-update），改用 GREATEST 单语句 SQL 从根本规避（chatsession 写回旧值仅瞬时偏差，下一条 ACK GREATEST 即纠正）

---

## 2. MAJOR 修复

### M1 — Push 服务关停 UAF

**问题**：`brpc::Server` 析构 (`SERVER_OWNS_SERVICE`) 会 delete `PushServiceImpl`，但 `_push_subscriber` 永不显式 stop，MQ 消费线程持续调用 `_push_service->onPushMessage` → use-after-free。

**修法**：

- `push/source/push_server.h::PushServer`：
  - 构造函数新增 `MQClient::ptr` / `Subscriber::ptr` 参数
  - `start()` 退出顺序：`RunUntilAskedToQuit` → `_push_subscriber.reset()` → `_mq_client.reset()` → `_ws_server->stop()` → `_ws_thread.join()` → `_rpc_server->Join()` → 析构时 delete impl
  - 借助 `~MQClient()` 内 `_async_thread.join()` 的强保证：MQClient 析构返回时 ev 线程已退出，`onMessage` 不再 fire
- `PushServerBuilder::build()` 用 `std::move` 把 `_service_discover` / `_reg_client` / `_rpc_server` / `_mq_client` / `_push_subscriber` 全部转移到 `PushServer`，避免 main 中 builder 栈对象拖住生命周期

**反复推敲点**：
1. 第一轮 cpp-reviewer 抓到 builder 仍持强引用 → 改 std::move
2. 第二轮确认 main 中 `psb` / `server` 析构序：`server` 先于 `psb` 析构（声明逆序），`_ws_server` 在 PushServer 之后析构是安全的

---

### M2 — WS per-conn send 串行化

**问题**：websocketpp `connection::send` 不是线程安全；MQ 消费线程 / brpc IO 线程 / WS asio 线程并发 send 同一 conn 会出现帧错乱 / crash。

**修法**：

- `push/source/connection.hpp::Client` 新增 `std::shared_ptr<std::mutex> send_mu`（默认初始化）
- `Connection::send_mutex(conn)` 接口：返回 conn 关联的 send 锁；连接已不存在返回 nullptr
- `push/source/push_server.h::_local_send`：
  - `auto mu = _connections->send_mutex(c); if(!mu) continue;`
  - `std::lock_guard<std::mutex> lock(*mu);` 后再 send

**关键论证**：
- `shared_ptr<mutex>` 通过 map 唯一持有，调用方 `send_mutex` 拿到的是引用计数副本，即使 conn 被 remove，锁不会析构
- `Connection::_mutex` (内部 map 锁) → `send_mu` (per-conn 锁) 锁顺序无环，无死锁
- 100K 连接 ~10MB 内存占用，可接受

---

### M3 — `PushOutbox` reaper 实现

**问题**：原 commit 有 `enqueue` / `peek` / `remove`，但**完全没有 reaper 线程消费**，失败的推送会永远积压在 Redis 直到 OOM。

**修法**：

- `common/data_redis.hpp::PushOutbox`：
  - `try_acquire_reaper_lease(owner, ttl)` 用 Lua 原子化（`SET NX EX OR (GET==owner THEN EXPIRE)`）；防止 GET+EXPIRE 两步式的 TOCTOU race 导致两实例同时持锁
  - `release_reaper_lease(owner)` Lua CAS：`IF GET==owner THEN DEL`
- `message/source/message_server.h::MessageServiceImpl`：
  - `start_outbox_reaper(owner)` 启动独立线程
  - 主循环：拿不到锁 sleep；拿到锁 peek(50)；**peek-then-zrem**（先把这一批从 ZSET 移走，避免异步 ack 滞后导致同批反复重投）；publish_confirm 失败回调里 enqueue 回去，score 重新取 `now_ts`（挤到队尾留退避空间）
  - 三个参数（reap_interval=5、lease_ttl=30、batch_limit=50）收成函数内 constexpr
  - `catch(std::exception)` 与 `catch(...)` 双兜底，防未知异常杀死 reaper 让租约空挂 30s
  - 退出前主动 `release_reaper_lease`
- `message/source/message_server.h::MessageServer::start()` 关停顺序：
  - `RunUntilAskedToQuit` → `stop_outbox_reaper()` → `_mq_client.reset()` → `_rpc_server->Join()` → impl 析构
  - `Builder::build()` 同样把强引用 `std::move` 到 server

**反复推敲点**：
1. 第一轮 cpp-reviewer 抓到 GET+EXPIRE / GET+DEL 的 TOCTOU → Lua 原子化
2. 第二轮抓到析构顺序：MQClient ev 线程会访问已 delete 的 impl → 显式 stop reaper + reset mq_client + brpc Join 三步
3. 第三轮指出失败回调用了 reaper 起始时间戳的 `now_ts`，会把失败项插回 ZSET 头部 hot-loop → 改成在回调内 `time(nullptr)` 重新取

---

### M4 — `lease_lost` watchdog

**问题**：`worker_id.hpp` 暴露 `lease_lost()` 但 transmite 内没人轮询；Snowflake 续期失败后 `_id_generator` 还在用已被别人占据的 worker_id 继续发号 → 雪花 ID 重号污染 message 主键。

**修法**：

- `transmite/source/transmite_server.h::TransmiteServer`：
  - 构造函数新增 `WorkerIdAllocator::ptr` 可选参数
  - `start()` 启动 watchdog 线程：每 1s 轮询 `lease_lost()`
  - 触发时：`unregister()` 让 etcd 立即摘流 → **`std::abort()`**
  - 析构 join watchdog

**关键论证**：
- 评审时反复推敲是 `Stop(0)` 还是 `abort()`：`brpc::Server::Stop(0)` 不打断 in-flight handler，已经在 `GetTransmitTarget` 内部跑到 `_id_generator->Next()` 之前/之中的请求会继续执行完，仍用旧 worker_id 发号 → 必须 `abort()` 让所有 bthread 一起死
- 1s 检测延迟是固有 cost（worker_id 续期 60s 一次，TTL 300s）；abort 不能消除"检测前"的重号窗口（这是自动分配方案的固有 cost），但能消除"检测后"的窗口

---

### M5 — 心跳触发未 ACK 真重发

**问题**：原代码 drain 出来只 LOG_DEBUG 不实际重发；drain 不做 zrem 导致同批反复扫；`max_age_sec=5` 与 `kUnackedTtl=7d` 错配 → 7 天后 unacked 整 key TTL 到期，消息彻底丢。

**修法**：

- `common/data_redis.hpp::UnackedPush`：
  - `peek_due` 替代 drain（仅查询，不删）
  - `bump_score(uid, seqs)` 用 `ZADD XX` 仅更新已存在项 score；同时 `expire(k, ttl)` 续 7 天
- `push/source/push_server.h::_on_heartbeat_resend`：
  - peek_due → 解析 user_seq 数值进 `unordered_set<uint64_t>` → 计算 `last_user_seq = min - 1`
  - **异步** `stub.GetOfflineMsg(...)`（用 `SelfDeleteRpcClosure`），不阻塞 WS asio 单线程
  - 回调里按 `mi.user_seq` 查 set 命中即重发；最后 bump_score
- `message/source/message_server.h::GetOfflineMsg`：
  - 建 `message_id → user_seq` map
  - 响应 `MessageInfo.user_seq` 填入；**不依赖列表下标**（`select_by_ids` 内部按 seq_id ASC 排序，与 timeline 的 user_seq 顺序不一致，按下标配对会跨会话错配）
- `push/source/push_server.cc`:
  - 新增 gflags `resend_batch=50` / `resend_max_age_sec=5`
  - `psb.set_resend_params(...)` 传给 PushServiceImpl
- `conf/push_server.conf` 写入默认值

**反复推敲点**：
1. 第一轮 cpp-reviewer 抓到 4 个必修：配对 by 下标错 / WS 阻塞同步 RPC / bump_score 不续 TTL / 参数未接 gflags → 全部修
2. 第二轮 PASS

---

## 3. 过度设计清理

### 3.1 删 `update(csm)` `SELECT FOR UPDATE` 兜底

**理由**：push 端走 `UPDATE GREATEST(last_ack_seq, n)` 原子单语句，永远以 DB 现值取最大；chatsession 全行 update 即使瞬时覆盖回旧值，下一条 ACK GREATEST 立即纠正。FOR UPDATE 这层是双重保险，性能代价（每次群属性修改都要拿行锁）大于收益。

**改动**：`common/mysql_chat_session_member.hpp::update(csm)` 恢复为最简单的单语句 update。

---

### 3.2 简化 `update_last_ack_seq` / `update_last_read_seq`

**理由**：原实现 `affected==0` 后再开新事务 SELECT 区分"行不存在 vs 等值不推进"，调用方对两种情况都不重试（已退群 / 幂等重复），区分无意义。

**改动**：
- 抽 `_atomic_advance_seq(column, ssid, uid, new_seq)` 私有助手，两个推进接口共用
- 删 `affected==0` 二次 SELECT，统一 return true

---

### 3.3 删 `UnackedPush::drain` 兼容别名

**理由**：原 drain 改成 peek_due 后，drain 保留为别名 — 但全代码已无调用方。无谓的兼容层。

**改动**：`common/data_redis.hpp` 删 drain。

---

### 3.4 关停日志合并为单行

**理由**：`"Push 关停: 1/3 ..."` `"2/3 ..."` `"3/3 ..."` 步骤式日志在每次正常关停都打 4 行 INFO，诊断价值不大。

**改动**：PushServer / MessageServer 各保留一行 `"X 关停完成"`。

---

### 3.5 删 reaper 重入保护 + 收常量

**理由**：`compare_exchange_strong` 防重入只在显式重复调用时触发；当前调用点只有 builder::make_rpc_object 一处，无重复调用风险。`reap_interval_sec` / `lease_ttl_sec` / `batch_limit` 是函数参数但调用方只传 owner，全用默认值 — conf 也没暴露。

**改动**：
- 删 `compare_exchange_strong`
- 三个参数收成函数内 `constexpr int kReapIntervalSec=5; kLeaseTtlSec=30; kBatchLimit=50;`
- 签名简化为 `start_outbox_reaper(owner)`

---

### 3.6 抽 `SelfDeleteRpcClosure` 到 common

**理由**：M1/B4 在 push_server.h 顶部加了 8 行模板类，被 PushBatch / UpdateAckSeq / GetOfflineMsg 三处复用。gateway `_pushNotify` 也需要它修栈对象 UAF。该类是通用 utility，应放在 common 下。

**改动**：新建 `common/brpc_closure.hpp`，push_server.h / gateway_server.h 改 include。

---

### 3.7 gateway `_pushNotify` UAF 修复

**理由**：cpp-reviewer 在 B3 顺手提到的预存 UAF：栈上 `brpc::Controller cntl / req / rsp` + `brpc::DoNothing` 异步回调访问已析构对象。

**改动**：`gateway/source/gateway_server.h::_pushNotify` 改用 `SelfDeleteRpcClosure<PushToUserReq, PushToUserRsp>`，on_done 失败时 LOG_WARN。

---

## 4. 历史遗留 bug

### 4.1 gateway `_redis_status` 死字段

**问题**：B3 删 `onClose` 后该字段无任何引用，但声明 + 初始化还在。

**改动**：删字段声明与初始化。

---

### 4.2 chatsession `update({csm, newcsm})` 编译错误

**问题**：DAO 没有接收 `std::initializer_list` / `std::vector` 的 `update` 重载，仅有单参版本。该行无法编译通过。

**改动**：`common/mysql_chat_session_member.hpp` 新增 `update(const std::vector<std::shared_ptr<ChatSessionMember>> &items)` 重载，同事务内对多行 `_db->update(*csm)`，保持转让群主原子性。任一失败抛异常 → trans 析构自动 rollback。

---

### 4.3 `MsgReadAck` 命名错 + 参数顺序错 + 错误未 return

**问题**：`chatsession_server.h:914` 调用 `update_last_read_msg(uid, ssid, msg_id)`：
- DAO 实际名是 `update_last_read_seq`（字段名 last_read_seq 才对应 last_read_seq 列）
- 参数顺序应为 `(ssid, uid, new_seq)`，传反了
- err_response 后未 `return`，后续 `response->set_success(true)` 把成功覆盖回去，HTTP 客户端拿到错误内容+success=true 的精分响应

**改动**：
```cpp
unsigned long read_seq = request->message_id();  // proto 字段名 message_id 但语义是 last_read_seq
bool ret = _mysql_chat_session_member->update_last_read_seq(ssid, uid, read_seq);
if (ret == false) {
    LOG_ERROR("请求ID - {} 更新会话成员 {} 已读位点失败", rid, uid);
    return err_response(rid, "更新会话成员已读位点失败");  // 加 return
}
```

---

### 4.4 `QuitChatSession` 调用不存在的 `remove(*csm)`

**问题**：`chatsession_server.h:885` `_mysql_chat_session_member->remove(*csm)`，DAO 仅有 `set_quit / remove_all`，无该重载。

**改动**：改为 `set_quit(ssid, uid)`（软删除，置 is_quit=true + quit_time + member_count -1）。

---

### 4.5 4 处 LOG_ERROR 占位符与参数数量不匹配

**问题**：spdlog/fmt 模式下，占位符与参数数量不匹配会抛异常或输出错乱。

**改动**：

| 行号 | 原状 | 修复 |
|---|---|---|
| 536 | `"...{}", rid, uid` 但只 1 个 `{}` | 占位 + 参数都改齐 `{} ... {}", rid` |
| 589 | `"用户没有权限更改管理员 {} 的权限", rid, uid, cuid` 占位 2 参数 3 | 改为 `"用户 {} 没有权限更改管理员 {}", rid, uid, cuid` |
| 727/762/802/836 | `"用户 {} 在会话 {}", rid, uid` 占位 3 参数 2 | 全部加 `, ssid` |
| 734 | `"用户 {} 更改在会话 {} 中..."` 占位 5 参数 0 | 改为 2 占位 + `rid, uid` |

---

## 5. 文件改动清单

| 文件 | 行变化 | 关键改动 |
|---|---|---|
| `proto/base.proto` | +2 | B1 新增 `MessageInfo.user_seq` |
| `proto/notify.proto` | +3 -1 | B1 N6 `NotifyMsgPushAck.message_id` 改 int64 |
| `proto/base.proto` | +3 | B1 新增 `MessageInfo.user_seq` |
| `conf/transmite_server.conf` | +3 -3 | B2 exchange 对齐 |
| `conf/gateway_server.conf` | +2 -1 | B3 ws port 注释化 |
| `conf/push_server.conf` | +3 | M5 重发参数 |
| `common/brpc_closure.hpp` | +35 | 新文件 |
| `common/data_redis.hpp` | +75 -6 | M3 reaper 租约 / M5 peek_due+bump_score |
| `common/mysql_chat_session_member.hpp` | +66 -20 | B4 GREATEST 原子 / 4.2 vector 重载 |
| `common/rabbitmq.hpp` | +21 -7 | B2 publisher-only declare / M1 ~MQClient 顺序 |
| `gateway/source/gateway_server.cc` | +7 -3 | B3 ws flag 兼容化 |
| `gateway/source/gateway_server.h` | +30 -108 | B3 删 WS / 4.1 死字段 / _pushNotify UAF 修复 |
| `message/source/message_server.cc` | +1 | M3 reaper owner |
| `message/source/message_server.h` | +166 -7 | B4 / M3 / M5 |
| `push/source/connection.hpp` | +12 | M2 send_mutex |
| `push/source/push_server.cc` | +5 | M5 gflags |
| `push/source/push_server.h` | +257 -32 | B1 / B4 / M1 / M2 / M5 |
| `transmite/source/transmite_server.cc` | +5 -2 | B2 注释强调 |
| `transmite/source/transmite_server.h` | +66 -8 | B2 / M4 |
| `chatsession/source/chatsession_server.h` | +18 -15 | 4.3 / 4.4 / 4.5 |

---

## 6. 验证

每一项修复都通过 `cpp-reviewer` 子代理独立 review：

| 项 | 复审次数 | 最终判定 |
|---|---|---|
| B1 | 1 | PASS（修复编译错误后 PASS） |
| B2 | 1 | PASS（修复 WARN-only 透传后 PASS） |
| B4 | 3 | PASS（解决 NewCallback 编译错 → 改 GREATEST → 解决 chatsession lost-update） |
| B3 | 1 | PASS（补 listen 返回值检查后 PASS） |
| M1 | 2 | PASS（改 std::move 后 PASS） |
| M2 | 1 | PASS |
| M3 | 2 | PASS（Lua 原子化 + 析构序 + now_ts 重新取 后 PASS） |
| M4 | 1 | PASS（改 Stop → abort 后 PASS） |
| M5 | 2 | PASS（4 项必修后 PASS） |
| 清理轮 | 1 | PASS |

---

## 7. 上线前注意事项

### 7.1 重启顺序

由于 message 服务现在显式校验 MQ exchange 配置，且 transmite 改成了 publisher-only：
1. 先重启 message（确保新代码下声明 queue + bind）
2. 再重启 transmite（exchange 已存在，仅 publish）
3. 再重启 gateway（与 push 解耦，先后顺序无要求）
4. 再重启 push

如果 transmite 先于 message 启动，新 exchange 已声明但无 binding，FANOUT 在 mandatory=false 下会静默丢消息（这是评审报告 B2 之外的相邻问题，不在本 PR 范围）。

### 7.2 客户端协议升级

- `MessageInfo.user_seq` 是 `optional` 字段，旧客户端可正常反序列化
- 客户端需要把 `NotifyMsgPushAck.message_id` 从 `string` 升级为 `int64`（之前类型不匹配，旧客户端的 ACK 反序列化本就一直失败）
- 客户端 ACK 实现需要按 `MessageInfo.user_seq` 字段填 `NotifyMsgPushAck.user_seq`；缺失（大群读扩散）时不应回 ACK

### 7.3 Redis key 新增

- `im:push:outbox:lock` — M3 reaper 单实例租约
- 无需手动初始化；首次 reaper 启动时 SET NX 创建

### 7.4 配置项变更

| 服务 | 新增 / 变更 | 默认值 |
|---|---|---|
| transmite | `mq_msg_exchange` | `chat_msg_exchange`（必须与 message 一致） |
| transmite | `mq_msg_queue` / `mq_msg_binding_key` | 留空 |
| push | `resend_batch` | 50 |
| push | `resend_max_age_sec` | 5 |
| gateway | `websocket_listen_port` | 0（已废弃） |

---

## 8. 后续遗留（v2.1 范围）

本 PR 不处理但应跟进：

| ID | 描述 | 来自 |
|---|---|---|
| B2.1 | transmite 先于 message 启动时 FANOUT 静默丢消息（mandatory=false） | B2 cpp-reviewer 提示 |
| M6 | 跨实例 PushBatch 失败仅 LOG_WARN，不入 outbox / unacked 带 payload | 原 review 报告 |
| M7 | 限流早于 Members 校验，可被伪造 uid 打爆受害人计数器 | 原 review 报告 |
| M8 | Members 缓存 read-and-compute 与成员变更存在竞态 | 原 review 报告 |
| M9 | 大群读/写扩散切换阈值跨越期会丢消息（GetRecentMsg 未 union 两条路径） | 原 review 报告 |
| M10 | Redis 不通时 fallback worker_id 与正常 incr slot 重叠 | 原 review 报告 |
| MINOR | N1-N9 / NIT-1~7 共 16 项 | 原 review 报告 |

---

**结论**：本 PR 修复了 v2.0 阻塞上线的 9 个核心问题（B1-B4 + M1-M5），并清理了引入的过度设计与若干历史遗留 bug。每项独立通过 cpp-reviewer 验证，可作为 v2.0 的上线候选基线。

如需进一步收尾，建议按"7.4 节"列的顺序在 v2.1 中渐进推进。
