# Transmite 服务重构设计

> **状态**: 设计完成
> **日期**: 2026-05-16
> **基线**: 3.0-dev @ fca5555
> **范围**: Transmite 服务（消息接入管线）完整重构，对齐新架构
> **依赖**: Identity / Conversation / Message 服务已重构完成

---

## 0. 背景

Transmite 是消息发送的核心入口服务，负责接收客户端发来的新消息，完成幂等检查、限流、ID 生成、成员拉取后投递 MQ 并返回。当前实现仍使用旧架构的 Stub 名称、旧 Proto、旧响应模式，需要对齐。

核心消息链路：

```
Client → Gateway → Transmite → MQ(FANOUT) → Message(落库) → MQ → Push(下发) → 对端 Client
```

---

## 1. Proto 重新设计

### 1.1 当前问题

- RPC 名为 `GetTransmitTarget`，名不副实（实际是发送消息）
- Req 中 `optional user_id` / `optional session_id` 应在 brpc metadata
- Rsp 中 `target_id_list` 是内部细节，不应暴露给调用方
- 响应未统一使用 `ResponseHeader`

### 1.2 新 Proto

```protobuf
// proto/transmite/transmite_service.proto
syntax = "proto3";
package chatnow.transmite;

option cc_generic_services = true;

import "common/envelope.proto";
import "message/message_types.proto";

service MsgTransmitService {
    rpc SendMessage(SendMessageReq) returns (SendMessageRsp);
}

message SendMessageReq {
    string request_id = 1;
    string conversation_id = 2;
    chatnow.message.MessageContent content = 3;
    string client_msg_id = 4;                           // 客户端幂等键
    optional chatnow.message.ReplyRef reply_to = 5;     // 回复引用
    repeated string mentioned_user_ids = 6;             // @提及
    optional chatnow.message.ForwardInfo forward_info = 7; // 转发来源
}

message SendMessageRsp {
    chatnow.common.ResponseHeader header = 1;
    chatnow.message.Message message = 2;                // 组装后的完整消息
}
```

### 1.3 变更要点

| 字段 | 变更 |
|------|------|
| `user_id` | 删除，从 brpc metadata `x-user-id` 提取 |
| `session_id` | 删除，JWT 体系下 device_id 在 metadata |
| `target_id_list` | 删除，调用方不需要推送目标列表 |
| `reply_to` / `mentioned_user_ids` / `forward_info` | 新增，对标主流 IM 发消息字段 |

---

## 2. 服务实现设计

### 2.1 Metadata 提取

```cpp
auto auth = extract_auth(cntl);    // 从 brpc metadata 读 x-user-id / x-device-id / x-trace-id
std::string uid = auth.user_id;    // 强校验：缺字段 → SYSTEM_INTERNAL_ERROR
```

### 2.2 Stub 切换

| 当前 | 改为 | 用途 |
|------|------|------|
| `UserService_Stub` | `IdentityService_Stub` → `GetMultiUserInfo` | 查 sender 信息 |
| `ConversationService_Stub` → `GetMemberIds` | 不变（已对齐） | 拿收件人列表 |
| `MsgStorageService_Stub` → `SelectByClientMsg` | `MessageService_Stub` → `SelectByClientMsgId` | 幂等去重 |

### 2.3 响应格式统一

```cpp
// 旧
response->set_success(false);
response->set_errmsg("...");

// 新
response->mutable_header()->set_success(false);
response->mutable_header()->set_error_code(chatnow::common::kXxx);
response->mutable_header()->set_error_message("...");
```

### 2.4 服务发现名称

Builder 中 `_user_service_name` → `"identity_service"`，对应 Identity 服务在 etcd 注册名。

### 2.5 不变的部分

- Snowflake + WorkerIdAllocator + lease watchdog（已验证）
- SeqGen（Redis INCR session_seq / user_seq）（已验证）
- RateLimiter（Redis 固定窗口）（功能正确）
- Members cache（Redis 优先 → RPC fallback + 回填）（已验证）
- MQ `publish_confirm` + trace_id 透传 MQ header（已验证）
- 大群 ≥200 读扩散分支（已验证）

---

## 3. 错误处理

### 3.1 错误码映射

| 场景 | 错误码 | 行为 |
|------|--------|------|
| metadata 缺 `x-user-id` | `SYSTEM_INTERNAL_ERROR` | 直接返回 |
| 文件消息缺 `file_id` | `INVALID_ARGUMENT` | 返回 "请先上传后再发送" |
| 用户级限流命中 | `RESOURCE_EXHAUSTED` | 返回 "rate_limited" |
| 会话级限流命中 | `RESOURCE_EXHAUSTED` | 同上 |
| client_msg_id 命中 | — | 正常返回旧消息 |
| Identity 服务不可达 | `SERVICE_UNAVAILABLE` | 返回 "依赖服务暂不可用" |
| Conversation 服务不可达 | `SERVICE_UNAVAILABLE` | 同上 |
| Message 服务不可达（幂等检查） | fail-open | 跳过幂等检查，继续发送 |
| 会话成员列表为空 | `FAILED_PRECONDITION` | 返回 "会话已解散或不存在" |
| sender 不在成员列表 | `PERMISSION_DENIED` | 返回 "无发消息权限" |
| Redis INCR 失败 | `SERVICE_UNAVAILABLE` | 返回 "服务繁忙" |
| MQ `publish_confirm` 失败/超时 | `SERVICE_UNAVAILABLE` | 返回 "消息发送失败，请重试" |

### 3.2 关键边界决策

- **幂等检查 fail-open**：Message 不可达时跳过幂等，宁重勿丢。client_msg_id 索引 + DB 约束兜底。
- **sender 成员校验**：从已拉回的 member_id_list 中检查，零额外成本。
- **大群读扩散**：≥200 人跳过 user_seq 生成和 user_timeline 写入，客户端按 seq_id 增量同步。
- **MQ confirm 超时**：不返回成功，客户端重试带同一 client_msg_id，幂等检查兜底。

---

## 4. 发号器不独立拆分

Snowflake 的 worker_id 通过 Redis 租约分配（已有 `WorkerIdAllocator` + watchdog），session_seq / user_seq 用 Redis INCR。多实例安全无需拆分独立服务，避免在 `SendMessage` 热路径增加额外 RPC 往返。

---

## 5. 文件变更清单

| 文件 | 改动 | 估计行数 |
|------|------|---------|
| `proto/transmite/transmite_service.proto` | 重写：`SendMessage` + 新 Req/Rsp | ~35 |
| `transmite/source/transmite_server.h` | Stub 切换 + metadata 提取 + 响应格式 + sender 成员校验 | ~60 |
| `conf/transmite_server.conf` | `user_service_name` → `identity_service_name` | ~2 |

无新文件，无新增 DAO，无 ODB 变更。

---

## 6. 依赖顺序

Transmite 依赖已完成的三个服务 proto stub：
- `identity/identity_service.proto` ✅
- `conversation/conversation_service.proto` ✅
- `message/message_service.proto` ✅

无反向依赖。Gateway 和 Push 会在后续重构中对齐 Transmite 的新 RPC 名。
