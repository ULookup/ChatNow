# Push 服务重构设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-17
> **基线**: 3.0-dev @ HEAD
> **范围**: Push 服务（消息推送 + WebSocket 连接管理）完整重构，对齐新架构；Push 与 Presence 拆分
> **依赖**: Identity / Message / Conversation / Media / Relationship / Transmite 已重构完成

---

## 0. 背景

Push 服务负责终结客户端 WebSocket 长连接，将消息推送到在线设备。当前实现仍使用旧架构的 session_id 鉴权、旧 Stub、旧响应格式、设备级路由缺失，需要对齐。

架构决策：**Push 与 Presence 拆分**。Push 只管 WS 连接、消息下发、unacked 缓冲、跨实例路由；Presence 管在线状态聚合、状态订阅、Typing 通知（后续独立 spec）。Push 在连接/断连/心跳时直接写 Redis presence 数据，Presence 服务只负责读取聚合。

核心消息链路：

```
Client → Gateway → Transmite → MQ(FANOUT) → Message(落库) → MQ → Push(下发) → WS → Client
```

---

## 1. WS 鉴权：首条消息带 JWT

采用 Discord/Teams 等主流 IM 标准做法——URL 不带参数，首条 WS 消息携带 access_token：

```
Client → WS Connect → Server accept（不鉴权）
Client → 首条 WS 消息：NotifyMessage { CLIENT_AUTH, access_token, device_id }
Server → JWT 验签 → 提取 sub(user_id), did(device_id), jti
       → 查 Redis im:jwt:revoked:{jti}
       → 命中 → 关闭连接
       → 通过 → 注册连接 + 写在线路由 + 写 Presence
```

比 URL query param 好的点：
- Token 不暴露在 URL（URL 会进 access log / proxy log）
- 可在同一 WS 连接上做 token 续期

Push 不在 Gateway 后面——WS 直连不走 Gateway，所以 Push 必须自己做 JWT 验签。

### 1.1 NotifyClientAuth 改造

```protobuf
message NotifyClientAuth {
    string access_token = 1;            // JWT access token（替代 session_id）
    string device_id = 2;              // 保留
    optional uint64 last_user_seq = 3; // 保留
}
```

---

## 2. Proto 变更

### 2.1 notify.proto

**NotifyClientAuth** — `session_id` → `access_token`（见 §1.1）。

**NotifyMsgPushAck** — 加 `device_id`（设备级 ACK）：

```protobuf
message NotifyMsgPushAck {
    string user_id = 1;
    string device_id = 2;              // 新增
    int64 message_id = 3;
    uint64 user_seq = 4;
    string conversation_id = 5;
}
```

**NotifyType** — 加 KICKED 相关 + 群解散：

```protobuf
enum NotifyType {
    // ... 已有 FRIEND_ADD_APPLY_NOTIFY ~ READ_RECEIPT_NOTIFY (0-10)
    KICKED_BY_NEW_DEVICE = 11;          // 同 platform 新设备登录踢旧
    KICKED_BY_REVOKE = 12;             // 用户主动吊销设备
    FORCE_LOGOUT = 13;                 // 安全策略强制下线
    CONVERSATION_DISMISSED_NOTIFY = 14; // 群解散通知

    CLIENT_AUTH = 49;
    MSG_PUSH_ACK = 50;
    CLIENT_HEARTBEAT = 51;
}
```

**NotifyKicked**：

```protobuf
message NotifyKicked {
    NotifyType reason = 1;
    string message = 2;                // 给人看的文案
}
```

NotifyMessage.oneof 加 `NotifyKicked kicked = 18;`。

### 2.2 push_service.proto

`PushToUserReq` 加 `target_device_ids`，Rsp 改为 `ResponseHeader`（已有 `option cc_generic_services = true` ✅）：

```protobuf
message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    chatnow.push.NotifyMessage notify = 3;
    optional uint64 user_seq = 4;
    repeated string target_device_ids = 5;   // 空=所有设备；非空=仅指定设备（KICKED 场景）
}

message PushToUserRsp {
    chatnow.common.ResponseHeader header = 1;
    int32 online_device_count = 2;
}

message PushBatchRsp {
    chatnow.common.ResponseHeader header = 1;
    int32 online_count = 2;
}
```

### 2.3 删除旧 proto/push.proto

旧 `proto/push.proto`（flat `chatnow` 包，老接口）— 全仓 grep 零引用后删除。

---

## 3. 设备级路由

### 3.1 Redis key 设计

```
im:online:{uid}                     HASH { device_id: instance_id }
im:online:device:{uid}:{device_id}  STRING instance_id  TTL 120s（心跳续期）
im:unacked:{uid}:{device_id}        ZSET score=user_seq member=payload_b64  TTL 7d
im:push:outbox                      ZSET score=timestamp 跨实例重试队列
im:presence:device:{uid}:{did}      HASH { state, platform, last_active_at_ms }  TTL 120s
```

### 3.2 连接表 key 改为 (uid, device_id)

```cpp
class Connection {
    // _uid_device_connections:  uid → device_id → set<conn_ptr>
    // _conn_clients:            conn → Client{uid, device_id, jwt_jti, last_active_ts, send_mu}

    void insert(conn, uid, device_id, jwt_jti);
    // 同 uid+device_id 已有连接 → 关闭旧连接，保留新连接

    std::vector<server_t::connection_ptr> connections(uid, device_id);
};
```

### 3.3 PushToUser / PushBatch 按设备级下发

```cpp
// PushToUser：查 im:online:{uid} → 拿到所有 device_id → 逐个 device 下发
// 每个 device 有独立的 unacked 缓冲
// target_device_ids 参数：空=所有设备，非空=仅指定设备（KICKED 场景）
```

### 3.4 UnackedPush 改造

旧 `UnackedPush` 只存 `(uid, user_seq, timestamp)`，心跳补送时需 RPC 拉消息内容。改为存 `(uid, device_id, user_seq, payload_b64, timestamp)`，热路径补送命中本地缓存或 Redis 直接取 payload，零 RPC。

```cpp
// 新 push 签名
void push(uid, did, user_seq, payload, now_ts);
// 新 peek_due 签名
std::vector<UnackedItem> peek_due(uid, did, batch, max_age_sec);
// UnackedItem 含 user_seq + payload_b64
```

### 3.5 user_seq 仍按用户全局（不按设备）

设备级是"路由维度"和"在线维度"，消息已读状态仍是用户维度。一份 user_seq，所有设备共享。这是行业默认（微信/Telegram/Slack）。

---

## 4. 服务实现设计

### 4.1 类替换

- **新类**：`namespace chatnow::push { class PushServiceImpl : public chatnow::push::PushService }`
- **旧类** `PushServiceImpl : public PushService`（flat `chatnow::`）整体删除，不留兼容壳
- 所有 handler 使用 `HANDLE_RPC` 宏

### 4.2 RPC 鉴权模式

| RPC | 模式 | 原因 |
|-----|------|------|
| `PushToUser` | `HANDLE_RPC` | 内部服务调用，需 metadata 鉴权 |
| `PushBatch` | `HANDLE_RPC` | 内部服务调用，需 metadata 鉴权 |

### 4.3 handler 范式（PushToUser）

```cpp
void PushToUser(::google::protobuf::RpcController* cntl_base,
                const PushToUserReq* req, PushToUserRsp* rsp,
                ::google::protobuf::Closure* done) override {
    HANDLE_RPC(cntl, req, rsp, {
        auto notify = req->notify();
        if (req->has_user_seq() &&
            notify.notify_type() == NotifyType::CHAT_MESSAGE_NOTIFY &&
            notify.has_new_message_info()) {
            notify.mutable_new_message_info()->mutable_message_info()
                ->set_user_seq(req->user_seq());
        }
        auto payload = notify.SerializeAsString();

        int delivered = 0;
        auto devices = _online_route->devices(req->user_id());
        for (const auto &did : devices) {
            if (req->target_device_ids_size() > 0 &&
                std::find(req->target_device_ids().begin(),
                          req->target_device_ids().end(), did) == req->target_device_ids().end())
                continue;
            if (_local_send(req->user_id(), did, payload) > 0) ++delivered;
        }

        if (req->has_user_seq() && _unacked) {
            for (const auto &did : devices) {
                _unacked->push(req->user_id(), did, req->user_seq(),
                               payload, time(nullptr));
            }
        }

        rsp->set_online_device_count(delivered);
    });
}
```

### 4.4 内部辅助方法

| 方法 | 用途 |
|------|------|
| `int _local_send(uid, did, payload)` | 本机 WS 直推，返回送达连接数 |
| `void _publish_kicked_(uid, did, reason)` | 给特定设备推送 KICKED 通知 |
| `std::string _resolve_jwt_user_(access_token)` | 验签 + 查黑名单，失败抛 ServiceError |

### 4.5 Stub 切换

| 位置 | 旧 | 新 |
|------|-----|-----|
| 心跳补送（查消息） | `MsgStorageService_Stub.GetOfflineMsg` | `chatnow::message::MessageService_Stub.SyncMessages` |
| ACK 上报 | `MsgStorageService_Stub.UpdateAckSeq` | `chatnow::message::MessageService_Stub.UpdateReadAck` |
| JWT 验签 | — | 本地 JwtCodec 验签 + 查 Redis 吊销表 |

### 4.6 MQ 消费

`onPushMessage` 反序列化从 `chatnow::InternalMessage` → `chatnow::message::internal::InternalMessage`（与 Message 服务迁移后包名一致）。

### 4.7 Presence 数据写入（Push 为写入端）

Push 在以下时机直接写 Redis presence（不调 RPC）：

| 事件 | Redis 写入 |
|------|-----------|
| WS 连接建立 | `HSET im:presence:device:{uid}:{did} state=ONLINE platform=X` + `EXPIRE 120` |
| WS 心跳 | `EXPIRE im:presence:device:{uid}:{did} 120` |
| WS 断开 | `DEL im:presence:device:{uid}:{did}` |

Presence 服务（后续独立）只负责读取聚合和推送变化通知，不参与热路径。

---

## 5. 错误处理

### 5.1 错误码映射

| 场景 | 错误码 | 行为 |
|------|--------|------|
| WS CLIENT_AUTH token 缺失 | `AUTH_TOKEN_INVALID` | 关闭连接 |
| JWT 验签失败 | `AUTH_TOKEN_INVALID` | 关闭连接 |
| JWT 黑名单命中 | `AUTH_TOKEN_INVALID` | 关闭连接 |
| metadata 缺 `x-user-id`（RPC） | `SYSTEM_INTERNAL_ERROR` | 返回错误 |
| MQ 反序列化失败 | — | NackDiscard |
| 跨实例 PushBatch 失败 | — | 入 CrossInstanceOutbox + ERROR 日志，不抛 |

### 5.2 关键边界决策

- **JWT 黑名单查在 Push 本地** — 每个 WS 连接建立时验签 + 查 Redis `im:jwt:revoked:{jti}`，不调 Identity RPC。吊销后已连接设备不下线（由 KICKED 通知主动踢）。
- **Unacked 缓冲 per-device** — A 设备 ACK 不影响 B 设备的未确认队列。心跳补送按设备维度触发。
- **心跳补送 fail-soft** — `SyncMessages` 不可达 → 记 WARN，不自旋重试。客户端下次心跳或重连时再补。
- **CrossInstanceOutbox 重试** — 每 5s 一批，分布式锁防多实例并发，失败后 5s 重入。
- **跨实例 PushBatch 异步发起** — `brpc::DoNothing` Closure，不阻塞 MQ 消费线程。

---

## 6. 心跳补送

当前心跳补送调 `GetOfflineMsg` → 改为调 `SyncMessages(after_seq=last_user_seq, limit=N)`。

流程不变：peek_due 拿成熟 unacked → 先查本地缓存 → 未命中走 RPC → 拿到消息按 message_id→user_seq 配对下发 → bump_score 续期。

---

## 7. Builder / Server / main

### 7.1 Builder 步骤

```cpp
class PushServerBuilder {
    void make_jwt_object(key_map, current_kid);       // 新增：JwtCodec 注入
    void make_redis_object(host, port, db, ...);       // OnlineRoute / UnackedPush / CrossInstanceOutbox
    void make_discovery_object(reg_host, base, message_service, push_service);
    void make_reg_object(reg_host, service_name, access_host);
    void make_mq_object(user, pwd, host, exchange, queue, binding_key);
    void make_ws_object(ws_port);
    void make_rpc_object(port, timeout, num_threads, ws_port);
    void set_resend_params(batch, max_age_sec);
    PushServer::ptr build();
};
```

### 7.2 启动流程（make_rpc_object 内）

1. brpc::Server.AddService(impl, SERVER_OWNS_SERVICE)
2. brpc::Server.Start(port)
3. WS server listen + start_accept — **在 MQ 订阅之前就绪**
4. MQ subscribe (onPushMessage callback)
5. CrossInstanceOutbox reaper 启动

### 7.3 停服顺序（已有，保留）

MQ ev 线程退 → WS stop → brpc Stop/Join → delete impl

---

## 8. 不做的事（YAGNI）

- ❌ Presence 服务本身（独立 spec，Push 只写 Redis presence 数据）
- ❌ Device 管理接口（`ListDevices` / `RevokeDevice` / `RenameDevice` 是 Identity 的）
- ❌ 离线消息推送（APNs / FCM — 无此需求）
- ❌ WS 消息压缩（permessage-deflate — 后续加）
- ❌ 消息优先级队列（所有消息当前等权）
- ❌ MQ 拓扑改动（Direct exchange + msg_push_queue 不变）
- ❌ 旧 `proto/push.proto` 兼容壳（开发阶段，直接删）
- ❌ GROUP READ_RECEIPT_NOTIFY 推送（YAGNI）

---

## 9. 文件变更清单

| 文件 | 动作 | 说明 | 估算 |
|------|------|------|------|
| `proto/push/notify.proto` | 修改 | CLIENT_AUTH 改 JWT；ACK 加 device_id；加 KICKED 类型 + NotifyKicked | ~35 |
| `proto/push/push_service.proto` | 修改 | Rsp 切 ResponseHeader | ~10 |
| `proto/push.proto` | 删除 | 旧 flat proto，全仓零引用后删 | — |
| `push/source/push_server.h` | **重写** | namespace chatnow::push；JWT 鉴权；设备级路由；Stub 切换 | ~900 |
| `push/source/push_server.cc` | 修改 | gflags 同步 | ~10 |
| `push/source/connection.hpp` | 修改 | 连接表按 (uid, device_id)；JWT 鉴权流程 | ~80 |
| `push/CMakeLists.txt` | 修改 | proto_files 同步 | ~10 |
| `conf/push_server.conf` | 修改 | gflag 同步 | ~10 |
| `common/dao/data_redis.hpp` | 修改 | UnackedPush / OnlineRoute key 改设备级 | ~40 |

**合计：~1095 行**

---

## 10. 验收标准

1. WS 连接用 JWT access_token 鉴权通过，旧 session_id 路径完全删除
2. PushToUser / PushBatch 响应用 `ResponseHeader`，不再用 `success/errmsg`
3. Unacked 缓冲按 `(uid, device_id)` 维度，A 设备 ACK 不影响 B 设备
4. 心跳补送调 `MessageService.SyncMessages`，不再调 `GetOfflineMsg`
5. ACK 上报调 `MessageService.UpdateReadAck`，不再调 `UpdateAckSeq`
6. MQ 消费反序列化 `chatnow::message::internal::InternalMessage`（非老 `chatnow::InternalMessage`）
7. CrossInstanceOutbox reaper 正常运作
8. 旧 `proto/push.proto` 全仓 grep 零命中后删除
9. `push/` 目录 grep `MsgStorageService_Stub` 零命中
10. KICKED 通知可被 Push 服务下发

---

## 11. 后续工作（不在本 spec 范围）

- **Presence 服务**（独立 spec）：Presence 查询 API、多设备聚合、状态订阅 + 变化推送、Typing 通知、INVISIBLE 模式
- **Conversation 解散通知**（`CONVERSATION_DISMISSED_NOTIFY`）— Push 迁移期补 notify.proto 枚举值 + Conversation 服务推送
- **MarkRead PRIVATE READ_RECEIPT_NOTIFY** — Conversation 服务 MarkRead 时推给对端
- **多实例 Push 服务 MQ 消费乱序**（暂时不处理，单实例消费足够）
