# ChatNow Proto 全面重构设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-14
> **基线**: main @ `55decee`
> **范围**: 全量 proto 文件重构、服务边界重划分、新功能字段设计

---

## 0. 动机

当前 proto/ 目录 11 个文件，存在以下结构性问题：

| 问题 | 详情 |
|------|------|
| base.proto 是 god file（190 行） | UserInfo、MessageInfo、ChatSessionInfo、InternalMessage、ESIndexEvent、FileUploadData 全塞在一个文件 |
| 无统一响应信封 | 每个 Rsp 有各自的 `success + errmsg`，错误只能是字符串 |
| 三种分页模式并存 | `start_time/over_time`、`msg_count`、`last_message_id + msg_count` |
| auth 字段重复 25+ 处 | `optional string user_id = 2; optional string session_id = 3;` |
| 缺少现代 IM 能力 | 无回复引用、撤回、reactions、@提及、转发、pin、草稿、黑名单 |
| MQ 内部消息与 API 类型混杂 | InternalMessage、ESIndexEvent 和 UserInfo 在同一文件 |
| 命名不统一 | `chat_session_id` vs `session_id`、`SPEECH` vs `AUDIO`、`mail_number` vs `phone` |

参考主流 IM（微信、Telegram、Discord、飞书）的 proto/IDL 设计，重新规划整个 proto 体系。

---

## 1. 设计原则

1. **Domain-driven**：按业务域组织 proto，每个域自包含（类型 + 服务定义）
2. **内外分离**：客户端 API（`*_service.proto`）与内部消息（`*_internal.proto`）严格分开
3. **统一信封**：所有响应共用 `ResponseHeader`，所有分页共用 `PageRequest/PageResponse`
4. **sender_id 替代 sender UserInfo**：消息只传 sender_id，客户端本地缓存解析
5. **Proto 独立于部署**：proto 文件边界不绑定进程边界（如 Presence proto 独立但和 Push 同进程）

---

## 2. 新 Proto 文件结构

```
proto/
├── common/
│   ├── types.proto             # 共享基础类型（UserInfo 等）
│   ├── error.proto             # 统一错误码
│   └── envelope.proto          # ResponseHeader、PageRequest/Response、TimeRange
│
├── identity/
│   └── identity_service.proto  # 认证 + 用户资料（← 原 user.proto）
│
├── relationship/
│   └── relationship_service.proto  # 好友 + 黑名单（← 原 friend.proto）
│
├── conversation/
│   └── conversation_service.proto  # 会话 + 群管理 + 个人设置（← 原 chatsession.proto）
│
├── message/
│   ├── message_types.proto     # Message、MessageContent、ReplyRef 等
│   ├── message_service.proto   # 客户端 RPC
│   └── message_internal.proto  # InternalMessage、ESIndexEvent（服务间 MQ）
│
├── presence/
│   └── presence_service.proto  # 在线状态 + 输入中（Push 内模块，proto 独立）
│
├── push/
│   ├── push_service.proto      # 内部 Push RPC（← 原 push.proto）
│   └── notify.proto            # 客户端通知类型（← 原 notify.proto）
│
├── media/
│   └── media_service.proto     # 文件 + 语音识别（← 原 file.proto + speech.proto）
│
└── transmite/
    └── transmite_service.proto  # 消息入口（← 原 transmite.proto）
```

共 12 个 proto 文件（6 新增 + 7 重构 − 7 删除）。每个域 import 的只有 `common/` 下的文件。

---

## 3. Common 公共层

### 3.1 common/types.proto

```protobuf
syntax = "proto3";
package chatnow.common;

message UserInfo {
    string user_id = 1;
    string nickname = 2;
    string bio = 3;            // 原名 description
    string phone = 4;          // 原名 mail
    string avatar_url = 5;     // 原名 bytes avatar → URL
}
```

### 3.2 common/envelope.proto

```protobuf
syntax = "proto3";
package chatnow.common;

// 所有响应消息的 field 1
message ResponseHeader {
    string request_id = 1;
    bool success = 2;
    int32 error_code = 3;      // 0 = OK，非零参见 error.proto
    string error_message = 4;  // 人类可读，仅调试用
}

// 游标分页（推荐方式）
message PageRequest {
    int32 limit = 1;           // 默认 20，最大 100
    string cursor = 2;         // 上次响应的 next_cursor，首次为空
}

message PageResponse {
    bool has_more = 1;
    string next_cursor = 2;    // 下一页请求填入 PageRequest.cursor
    int32 total_count = 3;     // 可选，可能近似
}

// 时间段（兼容历史查询、分析场景）
message TimeRange {
    int64 start_time_ms = 1;
    int64 end_time_ms = 2;
}
```

### 3.3 common/error.proto

```protobuf
syntax = "proto3";
package chatnow.common;

enum ErrorCode {
    OK = 0;

    // 1000-1999: 认证 / 身份
    AUTH_INVALID_CREDENTIALS = 1001;
    AUTH_TOKEN_EXPIRED = 1002;
    AUTH_TOKEN_INVALID = 1003;
    AUTH_USER_NOT_FOUND = 1004;
    AUTH_USER_ALREADY_EXISTS = 1005;
    AUTH_VERIFY_CODE_INVALID = 1006;
    AUTH_VERIFY_CODE_EXPIRED = 1007;

    // 2000-2999: 关系
    RELATIONSHIP_ALREADY_FRIENDS = 2001;
    RELATIONSHIP_NOT_FRIENDS = 2002;
    RELATIONSHIP_BLOCKED = 2003;
    RELATIONSHIP_REQUEST_PENDING = 2004;

    // 3000-3999: 会话
    CONVERSATION_NOT_FOUND = 3001;
    CONVERSATION_NOT_MEMBER = 3002;
    CONVERSATION_NO_PERMISSION = 3003;
    CONVERSATION_MEMBER_LIMIT = 3004;

    // 4000-4999: 消息
    MESSAGE_NOT_FOUND = 4001;
    MESSAGE_RECALL_TIMEOUT = 4002;
    MESSAGE_ALREADY_RECALLED = 4003;
    MESSAGE_CONTENT_INVALID = 4004;

    // 5000-5999: 媒体
    MEDIA_FILE_TOO_LARGE = 5001;
    MEDIA_UNSUPPORTED_FORMAT = 5002;
    MEDIA_UPLOAD_FAILED = 5003;

    // 6000-6999: Presence
    PRESENCE_USER_OFFLINE = 6001;

    // 8000-8999: 限流
    RATE_LIMIT_EXCEEDED = 8001;

    // 9000-9999: 系统
    SYSTEM_INTERNAL_ERROR = 9001;
    SYSTEM_UNAVAILABLE = 9002;
    SYSTEM_TIMEOUT = 9003;
}
```

---

## 4. 消息域（message/）

### 4.1 message/message_types.proto

```protobuf
syntax = "proto3";
package chatnow.message;

import "common/types.proto";

enum MessageType {
    MESSAGE_TYPE_UNSPECIFIED = 0;
    TEXT = 1;
    IMAGE = 2;
    FILE = 3;
    AUDIO = 4;           // 原名 SPEECH
    VIDEO = 5;           // 新增
    LOCATION = 6;        // 新增
    STICKER = 7;         // 新增
    SYSTEM_NOTICE = 8;   // 新增（群公告、入群通知等）
}

enum MessageStatus {
    MESSAGE_STATUS_NORMAL = 0;
    MESSAGE_STATUS_RECALLED = 1;
    MESSAGE_STATUS_DELETED = 2;
}

message TextContent { string text = 1; }
message ImageContent {
    string file_id = 1;
    int32 width = 2;
    int32 height = 3;
    string thumbnail_url = 4;
}
message FileContent {
    string file_id = 1;
    string file_name = 2;
    int64 file_size = 3;
    string mime_type = 4;
}
message AudioContent {
    string file_id = 1;
    int32 duration_sec = 2;
}
message VideoContent {
    string file_id = 1;
    int32 duration_sec = 2;
    int32 width = 3;
    int32 height = 4;
    string thumbnail_url = 5;
}
message LocationContent {
    double latitude = 1;
    double longitude = 2;
    string name = 3;
    string address = 4;
}
message StickerContent {
    string sticker_id = 1;
    string pack_id = 2;
}
message SystemNoticeContent {
    string text = 1;
    string notice_type = 2;  // "member_joined", "member_left", "group_renamed" 等
}

message MessageContent {
    MessageType type = 1;
    oneof body {
        TextContent text = 2;
        ImageContent image = 3;
        FileContent file = 4;
        AudioContent audio = 5;
        VideoContent video = 6;
        LocationContent location = 7;
        StickerContent sticker = 8;
        SystemNoticeContent notice = 9;
    }
}

message Message {
    // 标识
    int64 message_id = 1;
    string conversation_id = 2;

    // 内容
    MessageContent content = 3;

    // 发送者
    string sender_id = 4;

    // 时间
    int64 created_at_ms = 5;
    int64 edited_at_ms = 6;        // 0 = 未编辑

    // 同步序号
    uint64 seq_id = 7;             // 会话内序号
    uint64 user_seq = 8;           // 用户全局序号（推送用）
    string client_msg_id = 9;      // 客户端幂等 key

    // 状态
    MessageStatus status = 10;

    // 引用回复
    optional ReplyRef reply_to = 11;

    // @提及
    repeated string mentioned_user_ids = 12;

    // 转发
    optional ForwardInfo forward_info = 13;

    // Reactions
    repeated ReactionGroup reactions = 14;

    // Pin
    bool is_pinned = 15;
}

message ReplyRef {
    int64 replied_message_id = 1;
    string replied_sender_id = 2;
    MessageType replied_message_type = 3;
    string content_preview = 4;       // 前 50 字
    bool is_recalled = 5;
}

message ReactionGroup {
    string emoji = 1;
    int32 count = 2;
    repeated string recent_user_ids = 3;  // 最近 3 人
    bool self_reacted = 4;
}

message ForwardInfo {
    string forward_from_user_id = 1;
    int64 forward_at_ms = 2;
    string source_conversation_id = 3;
}

message MessagePreview {
    int64 message_id = 1;
    string sender_id = 2;
    MessageType message_type = 3;
    string content_preview = 4;
    int64 sent_at_ms = 5;
    MessageStatus status = 6;
}
```

### 4.2 message/message_service.proto

```protobuf
syntax = "proto3";
package chatnow.message;

import "common/envelope.proto";
import "message/message_types.proto";

service MessageService {
    // 增量同步（seq 游标，替代 GetOfflineMsg）
    rpc SyncMessages(SyncMessagesReq) returns (SyncMessagesRsp);

    // 时间段查询
    rpc GetHistory(GetHistoryReq) returns (GetHistoryRsp);

    // 批量查消息
    rpc GetMessagesById(GetMessagesByIdReq) returns (GetMessagesByIdRsp);

    // 全文搜索（ES）
    rpc SearchMessages(SearchMessagesReq) returns (SearchMessagesRsp);

    // 撤回（2 分钟内有效）
    rpc RecallMessage(RecallMessageReq) returns (RecallMessageRsp);

    // Reactions
    rpc AddReaction(AddReactionReq) returns (AddReactionRsp);
    rpc RemoveReaction(RemoveReactionReq) returns (RemoveReactionRsp);
    rpc GetReactions(GetReactionsReq) returns (GetReactionsRsp);

    // Pin
    rpc PinMessage(PinMessageReq) returns (PinMessageRsp);
    rpc UnpinMessage(UnpinMessageReq) returns (UnpinMessageRsp);
    rpc ListPinnedMessages(ListPinnedReq) returns (ListPinnedRsp);

    // 管理
    rpc DeleteMessages(DeleteMessagesReq) returns (DeleteMessagesRsp);
    rpc ClearConversation(ClearConversationReq) returns (ClearConversationRsp);

    // 内部接口
    rpc SelectByClientMsgId(SelectByClientMsgIdReq) returns (SelectByClientMsgIdRsp);
    rpc UpdateReadAck(UpdateReadAckReq) returns (UpdateReadAckRsp);
}
```

### 4.3 message/message_internal.proto

客户端永远不 import 此文件。

```protobuf
syntax = "proto3";
package chatnow.message.internal;

import "message/message_types.proto";

message UserSeqPair {
    string user_id = 1;
    uint64 user_seq = 2;
}

// Fat Message：MQ → DB consumer 落库用
message InternalMessage {
    Message message = 1;
    repeated string member_id_list = 2;
    repeated UserSeqPair user_seqs = 3;
    bool is_large_group = 4;
}

// ES 索引事件：DB commit 后投递独立队列
message ESIndexEvent {
    int64 message_id = 1;
    string conversation_id = 2;
    string sender_id = 3;
    string content_text = 4;
    int64 created_at_ms = 5;
    uint64 seq_id = 6;
    MessageType message_type = 7;
}
```

---

## 5. 会话域（conversation/）

### 5.1 conversation/conversation_service.proto

```protobuf
syntax = "proto3";
package chatnow.conversation;

import "common/envelope.proto";
import "message/message_types.proto";  // 仅 import MessagePreview

enum ConversationType {
    CONVERSATION_TYPE_UNSPECIFIED = 0;
    PRIVATE = 1;
    GROUP = 2;
    CHANNEL = 3;  // 频道/广播（未来）
}

enum ConversationStatus {
    CONVERSATION_NORMAL = 0;
    CONVERSATION_ARCHIVED = 1;
    CONVERSATION_DISMISSED = 2;
}

enum MemberRole {
    MEMBER = 0;
    ADMIN = 1;
    OWNER = 2;
}

message Conversation {
    string conversation_id = 1;
    ConversationType type = 2;
    string name = 3;
    string avatar_url = 4;
    optional string description = 5;
    int64 created_at_ms = 6;
    int32 member_count = 7;
    repeated string top_member_ids = 8;
    ConversationStatus status = 9;
    optional MessagePreview last_message = 10;
    optional SelfMemberInfo self = 11;   // 当前用户视角，列表场景填充
}

message SelfMemberInfo {
    MemberRole role = 1;
    int64 joined_at_ms = 2;
    bool is_muted = 3;
    bool is_pinned = 4;
    int64 pin_time_ms = 5;
    bool is_visible = 6;
    uint64 last_read_seq = 7;        // 未读计算游标
    uint64 unread_count = 8;         // 未读数
    optional string draft = 9;       // 草稿同步
}

service ConversationService {
    rpc ListConversations(ListConversationsReq) returns (ListConversationsRsp);
    rpc GetConversation(GetConversationReq) returns (GetConversationRsp);
    rpc CreateConversation(CreateConversationReq) returns (CreateConversationRsp);

    // 群管理
    rpc UpdateConversation(UpdateConversationReq) returns (UpdateConversationRsp);
    rpc DismissConversation(DismissConversationReq) returns (DismissConversationRsp);
    rpc AddMembers(AddMembersReq) returns (AddMembersRsp);
    rpc RemoveMembers(RemoveMembersReq) returns (RemoveMembersRsp);
    rpc TransferOwner(TransferOwnerReq) returns (TransferOwnerRsp);
    rpc ChangeMemberRole(ChangeMemberRoleReq) returns (ChangeMemberRoleRsp);
    rpc ListMembers(ListMembersReq) returns (ListMembersRsp);

    // 个人设置
    rpc SetMute(SetMuteReq) returns (SetMuteRsp);
    rpc SetPin(SetPinReq) returns (SetPinRsp);
    rpc SetVisible(SetVisibleReq) returns (SetVisibleRsp);
    rpc QuitConversation(QuitConversationReq) returns (QuitConversationRsp);

    // 已读 + 草稿
    rpc MarkRead(MarkReadReq) returns (MarkReadRsp);
    rpc SaveDraft(SaveDraftReq) returns (SaveDraftRsp);

    // 搜索 + 内部
    rpc SearchConversations(SearchConversationsReq) returns (SearchConversationsRsp);
    rpc GetMemberIds(GetMemberIdsReq) returns (GetMemberIdsRsp);
}
```

---

## 6. 身份认证域（identity/）

```protobuf
syntax = "proto3";
package chatnow.identity;

import "common/envelope.proto";
import "common/types.proto";

service IdentityService {
    // 认证
    rpc Register(RegisterReq) returns (RegisterRsp);
    rpc Login(LoginReq) returns (LoginRsp);
    rpc Logout(LogoutReq) returns (LogoutRsp);           // 新增
    rpc SendVerifyCode(SendVerifyCodeReq) returns (SendVerifyCodeRsp);
    rpc RefreshToken(RefreshTokenReq) returns (RefreshTokenRsp);  // 新增

    // 资料
    rpc GetProfile(GetProfileReq) returns (GetProfileRsp);
    rpc UpdateProfile(UpdateProfileReq) returns (UpdateProfileRsp);  // 合并 SetAvatar/Nickname/Description

    // 搜索
    rpc SearchUsers(SearchUsersReq) returns (SearchUsersRsp);

    // 内部
    rpc GetMultiUserInfo(GetMultiUserInfoReq) returns (GetMultiUserInfoRsp);
}

message RegisterReq {
    string request_id = 1;
    oneof credential {
        UsernamePassword username_pwd = 2;
        PhoneVerifyCode phone_code = 3;
    }
    string nickname = 4;
}

message LoginReq {
    string request_id = 1;
    oneof credential {
        UsernamePassword username_pwd = 2;
        PhoneVerifyCode phone_code = 3;
    }
    string device_id = 4;
    string device_name = 5;
}
```

关键变化：
- `Register`/`Login` 用 `oneof credential` 合并用户名密码和手机验证码两套流程
- `UpdateProfile` 一次改多字段，替代三个独立 RPC
- `UserInfo.avatar` 从 `bytes` 改为 `avatar_url`（URL 字符串）

---

## 7. 关系域（relationship/）

```protobuf
syntax = "proto3";
package chatnow.relationship;

import "common/envelope.proto";
import "common/types.proto";

service RelationshipService {
    // 好友
    rpc ListFriends(ListFriendsReq) returns (ListFriendsRsp);     // 分页
    rpc SendFriendRequest(SendFriendReq) returns (SendFriendRsp);
    rpc HandleFriendRequest(HandleFriendReq) returns (HandleFriendRsp);
    rpc RemoveFriend(RemoveFriendReq) returns (RemoveFriendRsp);
    rpc SearchFriends(SearchFriendsReq) returns (SearchFriendsRsp);

    // 黑名单（新增）
    rpc BlockUser(BlockUserReq) returns (BlockUserRsp);
    rpc UnblockUser(UnblockUserReq) returns (UnblockUserRsp);
    rpc ListBlockedUsers(ListBlockedReq) returns (ListBlockedRsp);

    // 待处理
    rpc ListPendingRequests(ListPendingReq) returns (ListPendingRsp);
}
```

---

## 8. Presence 域（presence/）

- **Proto 独立**：`presence/presence_service.proto`
- **运行时**：和 Push 同进程，作为 Push 的内部模块。心跳直接在 Push 内处理，无 RPC 开销
- **状态存储**：Redis（`im:presence:{uid}` HASH、`im:presence:devices:{uid}` SET 120s TTL、`im:presence:typing:{uid}` SET 10s TTL）

```protobuf
syntax = "proto3";
package chatnow.presence;

import "common/envelope.proto";

enum PresenceState {
    PRESENCE_UNSPECIFIED = 0;
    ONLINE = 1;
    AWAY = 2;
    BUSY = 3;
    OFFLINE = 4;
    INVISIBLE = 5;
}

message Presence {
    string user_id = 1;
    PresenceState state = 2;
    int64 last_active_at_ms = 3;
    repeated string active_device_types = 4;
    optional string custom_status = 5;
}

service PresenceService {
    rpc SetPresence(SetPresenceReq) returns (SetPresenceRsp);
    rpc GetPresence(GetPresenceReq) returns (GetPresenceRsp);
    rpc BatchGetPresence(BatchGetPresenceReq) returns (BatchGetPresenceRsp);
    rpc SubscribePresence(SubscribeReq) returns (SubscribeRsp);
    rpc UnsubscribePresence(UnsubscribeReq) returns (UnsubscribeRsp);
    rpc SendTyping(TypingReq) returns (TypingRsp);
}
```

注意：无 `Heartbeat` RPC。Push 收到 WS 心跳后本地更新 Presence 的 Redis key。

---

## 9. Push 域（push/）

```protobuf
// push/push_service.proto —— 内部 RPC（其它服务调 Push）
service PushService {
    rpc PushToUser(PushToUserReq) returns (PushToUserRsp);
    rpc PushBatch(PushBatchReq) returns (PushBatchRsp);
}
```

```protobuf
// push/notify.proto —— 客户端通知类型
// 新增 NotifyMessageRecalled、NotifyPresenceChange、NotifyTyping 等
```

---

## 10. Media 域（media/）

合并原 `file.proto` + `speech.proto`：

```protobuf
service MediaService {
    // 文件
    rpc UploadFile(UploadFileReq) returns (UploadFileRsp);
    rpc DownloadFile(DownloadFileReq) returns (DownloadFileRsp);
    rpc GetFileInfo(GetFileInfoReq) returns (GetFileInfoRsp);  // 新增

    // 语音
    rpc SpeechRecognition(SpeechRecognitionReq) returns (SpeechRecognitionRsp);
}
```

---

## 11. 服务拓扑

```
客户端 ──HTTP/WS──→ Gateway
                      │
      ┌───────────────┼───────────────┐
      │               │               │
  Transmite      Conversation     Identity
  (消息入口)      (会话管理)       (认证+资料)
      │               │               │
      │ MQ            │ brpc          │ brpc
      ▼               ▼               ▼
  Message          Relationship     Media
  (存储+搜索)      (好友+黑名单)    (文件+语音)
      │
      │ MQ
      ▼
  Push (WS长连接 + 在线路由 + Presence模块)
```

7 个服务：
| 服务 | 原名称 | 职责 |
|------|--------|------|
| Gateway | gateway | HTTP+WS 入口，鉴权，路由 |
| Transmite | transmite | 消息入口：幂等去重、限流、seq 分配、组装、入 MQ |
| Message | message | 消息落库、Timeline、ES 索引、历史/搜索/撤回/reactions/pin |
| Push | push | WS 长连接、在线路由、消息推送、ACK 重传、Presence 模块 |
| Conversation | chatsession | 会话/群管理、成员、权限、已读、草稿 |
| Identity | user | 注册/登录/验证码、资料管理、用户搜索 |
| Relationship | friend | 好友 CRUD、好友申请、黑名单 |
| Media | file + speech | 文件上传/下载、语音识别 |

---

## 12. 命名对齐表

| 旧名 | 新名 | 理由 |
|------|------|------|
| `chat_session_id` | `conversation_id` | 行业通用术语 |
| `chat_session_type` (uint32) | `ConversationType` (enum) | 类型安全 |
| `ChatSessionMemberRole` | `MemberRole` | 上下文已明确 |
| `description` (个人) | `bio` | 行业通用 |
| `mail` / `mail_number` | `phone` | 实名（字段存的是手机号） |
| `avatar` (bytes) | `avatar_url` (string) | 不传输二进制头像 |
| `SPEECH` | `AUDIO` | 行业通用 |
| `FriendAddProcess` | `HandleFriendRequest` | 更语义化 |
| `GetOfflineMsg` | `SyncMessages` | 更准确描述行为 |
| `DeleteTimelineMsg` | `DeleteMessages` | 简化 |

---

## 13. 迁移策略

分三个阶段执行：

**Phase 1 — 新增文件，不改旧文件**
1. 创建 `common/` 下的 `types.proto`、`error.proto`、`envelope.proto`
2. 创建 `message/message_types.proto`、`message_internal.proto`
3. 创建 `presence/presence_service.proto`
4. 在 CMakeLists.txt 中添加编译，验证无冲突

**Phase 2 — 逐域迁移**
5. `user.proto` → `identity/identity_service.proto`，旧文件保留 re-export
6. `friend.proto` → `relationship/relationship_service.proto`
7. `chatsession.proto` → `conversation/conversation_service.proto`
8. `message.proto` → `message/message_service.proto`
9. `file.proto` + `speech.proto` → `media/media_service.proto`
10. `push.proto` + `notify.proto` → `push/push_service.proto` + `push/notify.proto`

**Phase 3 — 清理**
11. 删除所有旧 proto 文件（`base.proto`, `user.proto`, `friend.proto`, `chatsession.proto`, `file.proto`, `speech.proto`, `gateway.proto`）
12. 更新所有 C++ 代码的 include 路径

---

## 14. 文件变更清单

| 操作 | 文件 |
|------|------|
| 新增 | `proto/common/types.proto` |
| 新增 | `proto/common/error.proto` |
| 新增 | `proto/common/envelope.proto` |
| 新增 | `proto/message/message_types.proto` |
| 新增 | `proto/message/message_service.proto` |
| 新增 | `proto/message/message_internal.proto` |
| 新增 | `proto/presence/presence_service.proto` |
| 重构 | `proto/identity/identity_service.proto` ← `user.proto` |
| 重构 | `proto/relationship/relationship_service.proto` ← `friend.proto` |
| 重构 | `proto/conversation/conversation_service.proto` ← `chatsession.proto` |
| 重构 | `proto/media/media_service.proto` ← `file.proto` + `speech.proto` |
| 重构 | `proto/push/push_service.proto` ← `push.proto` |
| 重构 | `proto/push/notify.proto` ← `notify.proto` |
| 重构 | `proto/transmite/transmite_service.proto` ← `transmite.proto` |
| 删除 | `proto/base.proto` |
| 删除 | `proto/user.proto` |
| 删除 | `proto/friend.proto` |
| 删除 | `proto/chatsession.proto` |
| 删除 | `proto/file.proto` |
| 删除 | `proto/speech.proto` |
| 删除 | `proto/gateway.proto` |

---

## 15. 后续扩展预留

当前设计中已预留、但不在本 spec 实施范围内的能力：

| 能力 | 预留方式 | 建议实施时机 |
|------|----------|-------------|
| 已读回执（多用户） | Message 增加 `read_by` 字段 | 单聊已读上线后 |
| 消息已送达回执 | Message 增加 `delivered_to` 字段 | 与 read receipt 一起 |
| 视频通话信令 | 新增 `call/` proto 域 | 需要 WebRTC 后端时 |
| 频道/广播 | `ConversationType.CHANNEL` | 群聊成熟后 |
| 端到端加密 | MessageContent 加 `encrypted_content` | 安全需求明确时 |
| 表情商店 | StickerContent 已有 `pack_id` | 运营需求 |
