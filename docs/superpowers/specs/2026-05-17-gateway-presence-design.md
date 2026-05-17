# Gateway 重构 + Presence 服务 设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-17
> **基线**: 3.0-dev @ HEAD
> **范围**: Gateway 精简为纯代理层（鉴权 + 转发）+ Presence 服务从零新建（状态聚合 + 订阅 + 推送）
> **依赖**: 所有后端服务已完成 ResponseHeader 改造

---

## 0. 总览架构

```
                          ┌──────────────┐
                          │   Nginx L7   │  TLS 终结 + 限流
                          │  (stream)    │
                          └──┬───────┬───┘
                             │       │
                    HTTP     │       │  WebSocket
                             ▼       ▼
                    ┌──────────────┐  ┌──────────────┐
                    │   Gateway    │  │  Push (已重构) │
                    │  (HTTP代理)   │  │  (WS终结)     │
                    │              │  │              │
                    │ JWT验签      │  │ WS JWT验签    │
                    │ 限流/超时     │  │ 消息下发      │
                    │ brpc转发     │  │ 心跳/ACK      │
                    └──────┬───────┘  └────┬───┬─────┘
                           │              │   │
                           │   brpc       │   │ 直写 Redis presence
                           ▼              ▼   ▼
              ┌──────────────────────────────────────────┐
              │              服务网格 (brpc)               │
              │                                          │
              │  Identity  Message  Conversation         │
              │  Relationship  Transmite  Media          │
              │                 Presence (新建)           │
              │                                          │
              └──────┬───────┬───────┬──────────────────┘
                     │       │       │
                     ▼       ▼       ▼
              ┌────────┐ ┌──────┐ ┌──────┐
              │ MySQL  │ │Redis │ │  MQ  │
              └────────┘ └──────┘ └──────┘
```

### 两个入口，各司其职

| | Gateway | Push |
|---|---|---|
| 协议 | HTTP/1.1 + HTTP/2 | WebSocket |
| 前置 | Nginx L7 (TLS终结) | Nginx L4 (stream直通) |
| 鉴权 | Authorization header → JWT | 首条 WS 消息 CLIENT_AUTH → JWT |
| 频率 | 低-中 | 高（持续长连接） |
| 做什么 | 请求-响应：登录、列表、搜索、发送 | 推送-确认：消息下发、心跳、状态 |

### Presence 数据流 (读/写分离)

```
写入侧（热路径 — Push 负责）：

  WS连接建立  ──→  HSET im:presence:device:{uid}:{did} state=ONLINE
  WS心跳     ──→  EXPIRE im:presence:device:{uid}:{did} 120
  WS断开     ──→  DEL im:presence:device:{uid}:{did}

读取侧（冷路径 — Presence 服务负责）：

  好友列表打开  ──→  Presence.BatchGetPresence  → Redis Pipeline 读
  状态变化     ──→  Presence 定时扫描  → 推给订阅者
  Typing      ──→  Presence.SendTyping → Redis TTL → 推给对端
```

---

## 1. Gateway 设计：纯代理层

### 1.1 设计目标

Gateway 只做三件事：**鉴权、限流、转发**。不读业务字段、不写业务逻辑。新增业务 RPC 只需改路由表。

文件从当前 2542 行缩到 ~350 行。

### 1.2 路由注册

```cpp
// 白名单路由（无需 JWT）
Route("/service/user/login", &IdentityService_Stub::Login)
    .auth(WHITELISTED);

Route("/service/user/register", &IdentityService_Stub::Register)
    .auth(WHITELISTED);

Route("/service/user/refresh_token", &IdentityService_Stub::RefreshToken)
    .auth(WHITELISTED);

// 高延迟路由
Route("/service/message/sync", &MessageService_Stub::SyncMessages)
    .auth(JWT_REQUIRED)
    .timeout(10000);

// 低延迟路由
Route("/service/transmite/new_message", &TransmiteService_Stub::NewMessage)
    .auth(JWT_REQUIRED)
    .timeout(1000);

// 默认路由
Route("/service/conversation/list", &ConversationService_Stub::ListConversations)
    .auth(JWT_REQUIRED);

Route("/service/conversation/create", &ConversationService_Stub::CreateConversation)
    .auth(JWT_REQUIRED);

Route("/service/relationship/add_friend", &RelationshipService_Stub::AddFriend)
    .auth(JWT_REQUIRED);

Route("/service/message/recall", &MessageService_Stub::RecallMessage)
    .auth(JWT_REQUIRED);

Route("/service/presence/batch_get", &PresenceService_Stub::BatchGetPresence)
    .auth(JWT_REQUIRED);

// ... 所有路由统一格式
```

**路由端点配置项**：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `auth` | `JWT_REQUIRED` | `WHITELISTED` 或 `JWT_REQUIRED` |
| `timeout_ms` | `3000` | brpc 调用超时 |
| `rate_limit` | 无限制 | 端点级限流（req/s/user） |

### 1.3 转发器模板

所有路由共享同一个 `forward<T>()` 泛型模板：

```cpp
template<typename Stub, typename Req, typename Rsp, typename Method>
void forward(const httplib::Request& httpreq, httplib::Response& httpres,
             const AuthInfo& auth, const std::string& svc_name,
             int timeout_ms, Method method)
{
    Req pb_req;
    Rsp pb_rsp;
    auto err = [&pb_rsp, &httpres](int32_t code, const std::string& msg) {
        auto* h = pb_rsp.mutable_header();
        h->set_success(false);
        h->set_error_code(code);
        h->set_error_message(msg);
        httpres.set_content(pb_rsp.SerializeAsString(), "application/x-protobuf");
    };

    // 1. 反序列化 body
    if (!pb_req.ParseFromString(httpreq.body)) {
        return err(kSystemInvalidArgument, "parse request body failed");
    }

    // 2. 选 channel
    auto ch = _channels->choose(svc_name);
    if (!ch) return err(kSystemUnavailable, "no backend available");

    // 3. 写 brpc metadata + 转发
    Stub stub(ch.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms);
    chatnow::gateway::apply_auth_to_brpc(httpreq, cntl, auth);
    (stub.*method)(&cntl, &pb_req, &pb_rsp, nullptr);

    // 4. 序列化响应
    if (cntl.Failed()) {
        return err((cntl.ErrorCode() == brpc::ERPCTIMEDOUT)
                       ? kSystemTimeout : kSystemUnavailable,
                   cntl.ErrorText());
    }
    httpres.set_content(pb_rsp.SerializeAsString(), "application/x-protobuf");
}
```

**关键设计选择**：

1. **同步转发** — Gateway 线程直接等 brpc 返回。QPS 承载力靠多线程 + httplib 线程池。简单、可调试
2. **不读业务字段** — Gateway 不解 proto body，反序列化只为了 brpc 传输
3. **端点级超时可配** — 按路由注册时指定
4. **HTTP/2 启用** — Nginx → Gateway 走 HTTP/2 多路复用

### 1.4 鉴权中间件

复用已有 `gateway_auth.hpp`。每个非白名单 handler 入口一行：

```cpp
AuthInfo a;
if (!jwt_authenticate(req, res, _jwt_codec, _jwt_store, /*whitelisted=*/false, a))
    return;  // 401 已写
```

JWT 验签流程不变：取 Authorization Bearer → HS256 验签 → 查 Redis 黑名单 → 写 AuthInfo。

### 1.5 限流

三层限流：

1. **Nginx 层** — `limit_req_zone`，全局 QPS 限流，防 DDoS
2. **Gateway 全局** — token bucket，每实例 10000 req/s 硬顶
3. **端点级** — 按路由配置覆盖，如 `NewMessage` 100 req/s/user

超限返回 `RATE_LIMIT_EXCEEDED`。

### 1.6 错误处理

| 场景 | HTTP 状态码 | error_code |
|------|-------------|------------|
| Authorization header 缺失 | 401 | `AUTH_TOKEN_INVALID` |
| JWT 验签失败 | 401 | `AUTH_TOKEN_INVALID` |
| JWT 过期 | 401 | `AUTH_TOKEN_EXPIRED` |
| body 反序列化失败 | 400 | `SYSTEM_INVALID_ARGUMENT` |
| 后端服务不可达 | 503 | `SYSTEM_UNAVAILABLE` |
| 后端 RPC 超时 | 504 | `SYSTEM_TIMEOUT` |
| 后端业务错误 | 200 | 透传后端 ResponseHeader |

---

## 2. Presence 服务设计

### 2.1 服务职责

- 在线状态聚合（多设备 → 单一状态）
- 状态订阅管理（谁关注谁）
- 状态变化检测 + 推送
- Typing 通知（后续迭代，协议预留）

### 2.2 Proto 定义

```protobuf
syntax = "proto3";
package chatnow.presence;

import "common/envelope.proto";
import "common/types.proto";

enum PresenceState {
    PRESENCE_UNSPECIFIED = 0;
    ONLINE = 1;
    AWAY = 2;
    BUSY = 3;
    OFFLINE = 4;
    INVISIBLE = 5;
}

message DevicePresence {
    string device_id = 1;
    chatnow.common.DevicePlatform platform = 2;
    PresenceState state = 3;
    int64 last_active_at_ms = 4;
}

message Presence {
    string user_id = 1;
    PresenceState aggregated_state = 2;
    int64 last_active_at_ms = 3;
    repeated DevicePresence devices = 4;
    optional string custom_status = 5;
}

service PresenceService {
    rpc GetPresence(GetPresenceReq) returns (GetPresenceRsp);
    rpc BatchGetPresence(BatchGetPresenceReq) returns (BatchGetPresenceRsp);
    rpc SubscribePresence(SubscribeReq) returns (SubscribeRsp);
    rpc UnsubscribePresence(UnsubscribeReq) returns (UnsubscribeRsp);
    rpc SendTyping(TypingReq) returns (TypingRsp);
}

message GetPresenceReq {
    string request_id = 1;
    string user_id = 2;                  // 查询目标
}
message GetPresenceRsp {
    chatnow.common.ResponseHeader header = 1;
    Presence presence = 2;
}

message BatchGetPresenceReq {
    string request_id = 1;
    repeated string user_ids = 2;
}
message BatchGetPresenceRsp {
    chatnow.common.ResponseHeader header = 1;
    map<string, Presence> presences = 2;
}

message SubscribeReq {
    string request_id = 1;
    // subscriber_user_id 从 metadata 取
    repeated string subscribe_user_ids = 2;
}
message SubscribeRsp { chatnow.common.ResponseHeader header = 1; }

message UnsubscribeReq {
    string request_id = 1;
    // subscriber_user_id 从 metadata 取
    repeated string unsubscribe_user_ids = 3;
}
message UnsubscribeRsp { chatnow.common.ResponseHeader header = 1; }

message TypingReq {
    string request_id = 1;
    string conversation_id = 2;
    bool is_typing = 3;
    // user_id、device_id 从 metadata 取
}
message TypingRsp { chatnow.common.ResponseHeader header = 1; }
```

**Proto 变更要点**：
- 所有 Req 中的 `user_id`（调用方身份）从 metadata 取，删掉 Request 中的 `user_id` 字段
- `GetPresenceReq.user_id` / `BatchGetPresenceReq.user_ids` 保留（查询目标，不是调用方）
- `SetPresence` 移除 — 由 Push 直写 Redis，Presence 服务不设写入 RPC
- 增加 `DevicePresence` 消息，支持平台枚举

### 2.3 Redis Key 设计

```
im:presence:device:{uid}:{did}    HASH { state, platform, last_active_at_ms }  TTL 120s
im:presence:sub:{uid}              SET  subscriber_user_ids  (谁关注了 uid)
im:presence:typing:{conv_id}       SET  {uid}:{typing_since_ms}  TTL 10s
```

### 2.4 多设备状态聚合

```
aggregated_state = max(state of all devices)
  ONLINE(1) > BUSY(3) > AWAY(2) > INVISIBLE(5) > OFFLINE(4)
  (数字小的优先，INVISIBLE 对外显示 OFFLINE)
```

```cpp
Presence aggregate(const std::string& uid) {
    // 1. pipeline HGETALL im:presence:device:{uid}:*
    // 2. 过滤 TTL 过期的 key（120s 容忍窗）
    // 3. aggregated_state = max(state)
    // 4. last_active_at_ms = max(所有设备 last_active_at_ms)
    // 5. INVISIBLE 设备不写入 devices 列表（对外隐藏）
    return { uid, aggregated_state, last_active_at_ms, {devices...} };
}
```

### 2.5 订阅自动建立

好友关系建立时，**Relationship 服务**调 Presence 建立双向订阅：

```
AddFriend(A, B):
  1. Relationship.AddFriend(A, B)
  2. Presence.SubscribePresence(A, subscribe_user_ids=[B])
  3. Presence.SubscribePresence(B, subscribe_user_ids=[A])
```

好友关系删除时对应调 Unsubscribe。

### 2.6 状态变化检测与推送

采用**定时轮询**模式（简单可靠）：

```
每 5 秒:
  1. 扫描 im:presence:device:* 的变化集（对比上次快照）
  2. 对每个变化的 uid:
     a. 查 im:presence:sub:{uid} 获取订阅者列表
     b. 调用 Push.PushToUser → NotifyPresenceChange
```

5s 延迟对在线状态完全可接受（行业标准：微信 ~3s，Telegram ~5s）。

### 2.7 服务实现骨架

```cpp
namespace chatnow::presence {

class PresenceServiceImpl : public chatnow::presence::PresenceService {
public:
    PresenceServiceImpl(std::shared_ptr<RedisClient> redis,
                        std::shared_ptr<chatnow::push::PushService_Stub> push_stub);

    void GetPresence(::google::protobuf::RpcController* cntl_base,
                     const GetPresenceReq* req, GetPresenceRsp* rsp,
                     ::google::protobuf::Closure* done) override {
        HANDLE_RPC(cntl, req, rsp, {
            auto p = _aggregator->aggregate(req->user_id());
            *rsp->mutable_presence() = p;
        });
    }

    void BatchGetPresence(::google::protobuf::RpcController* cntl_base,
                          const BatchGetPresenceReq* req, BatchGetPresenceRsp* rsp,
                          ::google::protobuf::Closure* done) override {
        HANDLE_RPC(cntl, req, rsp, {
            for (const auto& uid : req->user_ids()) {
                (*rsp->mutable_presences())[uid] = _aggregator->aggregate(uid);
            }
        });
    }

    void SubscribePresence(::google::protobuf::RpcController* cntl_base,
                           const SubscribeReq* req, SubscribeRsp* rsp,
                           ::google::protobuf::Closure* done) override {
        HANDLE_RPC(cntl, req, rsp, {
            for (const auto& target_uid : req->subscribe_user_ids()) {
                _redis->sadd("im:presence:sub:" + target_uid, auth.user_id);
            }
        });
    }

    // ... UnsubscribePresence, SendTyping 类似

    void start_change_scanner();  // 启动定时扫描线程

private:
    std::shared_ptr<RedisClient> _redis;
    std::unique_ptr<PresenceAggregator> _aggregator;
    std::shared_ptr<chatnow::push::PushService_Stub> _push_stub;
};

}  // namespace chatnow::presence
```

### 2.8 Builder / Server / main

```cpp
class PresenceServerBuilder {
    void make_redis_object(host, port, db);
    void make_discovery_object(reg_host, base, push_service);
    void make_reg_object(reg_host, service_name, access_host);
    void make_change_scanner(interval_sec);
    PresenceServer::ptr build();
};

// main:
//   builder.make_* → build() → start() → wait()
```

---

## 3. 文件变更清单

### Gateway

| 文件 | 动作 | 说明 | 估算 |
|------|------|------|------|
| `gateway/source/gateway_server.h` | **重写** | 路由注册 + 转发器 + 端点配置 | ~350 行 |
| `gateway/source/gateway_auth.hpp` | 保留 | 已符合新设计 | — |
| `gateway/source/gateway_trace.hpp` | 保留 | 已符合新设计 | — |
| `gateway/source/gateway_server.cc` | 修改 | main 适配 | ~20 行 |

### Presence（新建）

| 文件 | 动作 | 说明 | 估算 |
|------|------|------|------|
| `presence/source/presence_server.h` | **新建** | PresenceServiceImpl + Aggregator + Builder | ~400 行 |
| `presence/source/presence_server.cc` | 新建 | main | ~20 行 |
| `presence/CMakeLists.txt` | 新建 | 构建配置 | ~40 行 |
| `conf/presence_server.conf` | 新建 | gflags 配置 | ~15 行 |

### Proto

| 文件 | 动作 | 说明 | 估算 |
|------|------|------|------|
| `proto/presence/presence_service.proto` | 修改 | 删 SetPresence；Request user_id 从 metadata 取；加 DevicePresence | ~30 行 |

---

## 4. 验收标准

### Gateway
1. `gateway_server.h` grep `user_id` / `session_id` 只出现在 auth / metadata 层，不在业务路由
2. 新增业务 RPC 只需改路由表，不改转发逻辑
3. 端点超时配置对 `SyncMessages` (10s) / `NewMessage` (1s) 生效
4. 非白名单路由缺 Authorization → 401
5. JWT 过期 → 401 + `AUTH_TOKEN_EXPIRED`
6. 后端不可达 → 503 + `SYSTEM_UNAVAILABLE`

### Presence
7. `BatchGetPresence` 返回多设备聚合后正确状态
8. 聚合优先级：ONLINE > BUSY > AWAY > INVISIBLE > OFFLINE
9. `SubscribePresence` 正确写入 `im:presence:sub:{uid}` SET
10. 状态变化 5s 内推送到订阅者
11. Push 写入 Redis presence 数据后，Presence 能读取并返回在线状态

---

## 5. 不做的事 (YAGNI)

- ❌ Gateway 异步转发（同步够用，QPS 靠多线程）
- ❌ Presence Pub/Sub 模式（先轮询，5s 延迟可接受）
- ❌ Typing 通知实现（proto 预留，后续迭代）
- ❌ 自定义状态文案 (`custom_status` 预留字段，不实现读写)
- ❌ Presence 分布式分片（单实例够用，后续按需加 `im:presence:sub:{uid}` 一致性哈希）
- ❌ Gateway 文件上传/下载转发（Media 服务客户端直传 MinIO，不经 Gateway）
- ❌ 旧 `proto/gateway.proto` 兼容（开发阶段，直接删）
