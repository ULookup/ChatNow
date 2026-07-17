# Typing 通知 — 服务端设计

## 概述

完成"对方正在输入..."功能的服务端链路，使 Client A 调用 `SendTyping` 后，Client B 通过 WebSocket 收到 `TYPING_NOTIFY`。

## 范围

- **仅限 PRIVATE（单聊）会话**。GROUP/CHANNEL 不处理。
- **仅服务端**，不涉及客户端 UI 逻辑。

## 现有基础

| 组件 | 状态 |
|---|---|
| Proto: `TypingReq`, `TypingRsp`, `NotifyTyping`, `TYPING_NOTIFY` | 已定义 |
| Presence: `SendTyping` 写 Redis `im:presence:typing:{conv_id}` | 已实现 |
| Push: `PushToUser` WebSocket 下发 | 已实现 |
| Push: `PushToUser` 透传任意 NotifyMessage | 已实现，无需改动 |
| Gateway: `/service/presence/send_typing` 路由 | **未实现** |

## 数据流

```
Client A                     Presence                    Push                    Client B
   │                            │                         │                        │
   │──POST /send_typing────────►│                         │                        │
   │  {conv_id, is_typing=true} │                         │                        │
   │                            │──SADD + EXPIRE 5s───────│                        │
   │                            │                         │                        │
   │                            │──PushToUser(B, Notify──►│                        │
   │                            │   Typing {A, conv,      │──WS binary frame──────►│
   │                            │    is_typing=true})     │                        │
```

## 改动点

### 1. Presence 服务 `SendTyping` 改造

文件：`presence/source/presence_server.h`

在现有 Redis 写入之后新增推送逻辑：

1. 解析 `conversation_id`：PRIVATE 会话 ID 格式为 `p_{lo_uid}_{hi_uid}`（见 `private_id_of_`），提取两个 uid
2. 找到非 caller 的 uid 作为 target
3. 构造 `NotifyTyping { user_id=caller, conversation_id, is_typing }`
4. 通过 `PushService_Stub::PushToUser` 推送
5. Redis TTL 从 10s 改为 5s
6. 非 `p_` 前缀的 conversation_id 静默跳过（GROUP/CHANNEL）

处理逻辑：

<pre>
SendTyping(conv_id, is_typing):
  # 现有：写/删 Redis
  if is_typing:
    SADD im:presence:typing:{conv_id} "{uid}:{now_ms}"
    EXPIRE im:presence:typing:{conv_id} 5s
  else:
    SREM im:presence:typing:{conv_id} "{uid}:*"

  # 新增：仅 PRIVATE 会话下发
  if not conv_id.starts_with("p_"):
    return  # GROUP/CHANNEL，不处理

  # 解析 p_{lo}_{hi} 找出对方 uid
  target = (lo == caller) ? hi : lo

  # 构造并推送
  notify = NotifyMessage { type=TYPING_NOTIFY, typing={ caller, conv_id, is_typing } }
  stub.PushToUser(target_uid=target, notify)
</pre>

### 2. Gateway 路由注册

文件：`gateway/source/gateway_server.h`

沿用现有 Presence 路由模式，新增：

```cpp
route<pres::PresenceService_Stub, pres::TypingReq, pres::TypingRsp>(
    "/service/presence/send_typing", _presence_svc, GatewayAuth::JWT_REQUIRED,
    &pres::PresenceService_Stub::SendTyping);
```

### 3. OpenAPI 文档更新

文件：`docs/api/openapi-presence.yaml`

在 `paths` 下新增 `/service/presence/send_typing`，在 `components/schemas` 下新增 `TypingReq`：

```yaml
  /service/presence/send_typing:
    post:
      summary: 发送输入状态
      description: user_id 从 JWT metadata 提取。仅 PRIVATE 会话生效，GROUP/CHANNEL 静默忽略。
      tags: [Presence]
      x-protobuf: { service: chatnow.presence.PresenceService, rpc: SendTyping, request: TypingReq, response: TypingRsp, file: proto/presence/presence_service.proto }
      requestBody:
        required: true
        content:
          application/x-protobuf:
            schema: { $ref: '#/components/schemas/TypingReq' }
      responses:
        '200': { $ref: '#/components/responses/Success' }
        '4xx': { $ref: '#/components/responses/BusinessError' }
```

Schemas 新增：

```yaml
    TypingReq:
      x-protobuf: { message: TypingReq, file: proto/presence/presence_service.proto }
      type: object
      required: [request_id, conversation_id, is_typing]
      properties:
        request_id: { type: string }
        conversation_id: { type: string }
        is_typing: { type: boolean }
```

### 4. 无需改动

- **Push 服务**：`PushToUser` 已存在；`NotifyTyping` 包含在 `NotifyMessage` oneof 中，`PushToUser` 透传任意 `NotifyMessage`，无需特殊处理
- **Proto**：`TypingReq`、`TypingRsp`、`NotifyTyping`、`TYPING_NOTIFY` 均已定义
- **Redis key**：`im:presence:typing:{conv_id}` 已存在

## 协议行为

- `is_typing=true`：开始输入，对端显示"正在输入..."，5 秒内无刷新则自动消失
- `is_typing=false`：停止输入（用户发消息或主动取消），对端立即消除
- 客户端需每 3-5 秒刷新 `is_typing=true` 以保持显示

## 边界情况

| 场景 | 处理 |
|---|---|
| GROUP/CHANNEL 会话 | conversation_id 非 `p_` 前缀，静默跳过 |
| 对方不在线 | `PushToUser` 返回 `online_device_count=0`，正常 |
| 客户端崩溃（未发送 is_typing=false） | Redis TTL 5s 后自动清理，对端超时自消 |
| 发消息后继续输入 | 消息到达 → 对端消除 typing → 新 typing 到达 → 对端重新显示 |

## 测试

- Presence `SendTyping` 单测：验证 PRIVATE 会话写入 Redis + 调用 PushToUser；GROUP 会话仅写 Redis 不推送
- Gateway 路由测试：验证 `/service/presence/send_typing` 正确转发
- OpenAPI 文档：验证 `openapi-presence.yaml` 包含 SendTyping 端点及 TypingReq schema
