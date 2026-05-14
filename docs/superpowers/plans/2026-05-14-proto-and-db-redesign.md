# Proto & DB 全面重构 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 proto/ 文件结构（12 文件、7 服务域）并同步更新 ODB 表（2 新表、3 改表）和 Redis 缓存（4 新 key）

**Architecture:** 分三阶段执行——Phase 1 新增文件不破坏现有代码，Phase 2 逐域迁移 proto 并同步改 ODB，Phase 3 删除旧文件并统一 include 路径

**Tech Stack:** Protobuf 3, ODB (C++ ORM), Redis, brpc

**依赖 Spec:**
- `docs/superpowers/specs/2026-05-14-proto-redesign.md`
- `docs/superpowers/specs/2026-05-14-db-redis-changes.md`

---

### Task 1: 创建 common/ 公共 proto 文件

**Files:**
- Create: `proto/common/types.proto`
- Create: `proto/common/error.proto`
- Create: `proto/common/envelope.proto`

- [ ] **Step 1: 创建 proto/common/types.proto**

```protobuf
syntax = "proto3";
package chatnow.common;

message UserInfo {
    string user_id = 1;
    string nickname = 2;
    string bio = 3;
    string phone = 4;
    string avatar_url = 5;
}
```

- [ ] **Step 2: 创建 proto/common/error.proto**

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

- [ ] **Step 3: 创建 proto/common/envelope.proto**

```protobuf
syntax = "proto3";
package chatnow.common;

message ResponseHeader {
    string request_id = 1;
    bool success = 2;
    int32 error_code = 3;
    string error_message = 4;
}

message PageRequest {
    int32 limit = 1;
    string cursor = 2;
}

message PageResponse {
    bool has_more = 1;
    string next_cursor = 2;
    int32 total_count = 3;
}

message TimeRange {
    int64 start_time_ms = 1;
    int64 end_time_ms = 2;
}
```

- [ ] **Step 4: 验证编译**

```bash
# 在 CMakeLists.txt 临时添加新 proto 的编译目标
cd /Users/yanghaoyang/repo/ChatNow && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
# Expected: 无 proto 编译错误
```

- [ ] **Step 5: 提交**

```bash
git add proto/common/types.proto proto/common/error.proto proto/common/envelope.proto
git commit -m "feat: 新增 common/ 公共 proto 文件（types, error, envelope）"
```

---

### Task 2: 创建 message/ 域 proto 文件

**Files:**
- Create: `proto/message/message_types.proto`
- Create: `proto/message/message_internal.proto`
- Create: `proto/message/message_service.proto`

- [ ] **Step 1: 创建 proto/message/message_types.proto**

完整的 message 域类型定义，包括 MessageType、MessageStatus、各 Content 类型、Message、ReplyRef、ReactionGroup、ForwardInfo、MessagePreview：

```protobuf
syntax = "proto3";
package chatnow.message;

import "common/types.proto";

enum MessageType {
    MESSAGE_TYPE_UNSPECIFIED = 0;
    TEXT = 1;
    IMAGE = 2;
    FILE = 3;
    AUDIO = 4;
    VIDEO = 5;
    LOCATION = 6;
    STICKER = 7;
    SYSTEM_NOTICE = 8;
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
    string notice_type = 2;
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
    int64 message_id = 1;
    string conversation_id = 2;
    MessageContent content = 3;
    string sender_id = 4;
    int64 created_at_ms = 5;
    int64 edited_at_ms = 6;
    uint64 seq_id = 7;
    uint64 user_seq = 8;
    string client_msg_id = 9;
    MessageStatus status = 10;
    optional ReplyRef reply_to = 11;
    repeated string mentioned_user_ids = 12;
    optional ForwardInfo forward_info = 13;
    repeated ReactionGroup reactions = 14;
    bool is_pinned = 15;
}

message ReplyRef {
    int64 replied_message_id = 1;
    string replied_sender_id = 2;
    MessageType replied_message_type = 3;
    string content_preview = 4;
    bool is_recalled = 5;
}

message ReactionGroup {
    string emoji = 1;
    int32 count = 2;
    repeated string recent_user_ids = 3;
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

- [ ] **Step 2: 创建 proto/message/message_internal.proto**

```protobuf
syntax = "proto3";
package chatnow.message.internal;

import "message/message_types.proto";

message UserSeqPair {
    string user_id = 1;
    uint64 user_seq = 2;
}

message InternalMessage {
    Message message = 1;
    repeated string member_id_list = 2;
    repeated UserSeqPair user_seqs = 3;
    bool is_large_group = 4;
}

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

- [ ] **Step 3: 创建 proto/message/message_service.proto**

```protobuf
syntax = "proto3";
package chatnow.message;

import "common/envelope.proto";
import "message/message_types.proto";

service MessageService {
    rpc SyncMessages(SyncMessagesReq) returns (SyncMessagesRsp);
    rpc GetHistory(GetHistoryReq) returns (GetHistoryRsp);
    rpc GetMessagesById(GetMessagesByIdReq) returns (GetMessagesByIdRsp);
    rpc SearchMessages(SearchMessagesReq) returns (SearchMessagesRsp);
    rpc RecallMessage(RecallMessageReq) returns (RecallMessageRsp);
    rpc AddReaction(AddReactionReq) returns (AddReactionRsp);
    rpc RemoveReaction(RemoveReactionReq) returns (RemoveReactionRsp);
    rpc GetReactions(GetReactionsReq) returns (GetReactionsRsp);
    rpc PinMessage(PinMessageReq) returns (PinMessageRsp);
    rpc UnpinMessage(UnpinMessageReq) returns (UnpinMessageRsp);
    rpc ListPinnedMessages(ListPinnedReq) returns (ListPinnedRsp);
    rpc DeleteMessages(DeleteMessagesReq) returns (DeleteMessagesRsp);
    rpc ClearConversation(ClearConversationReq) returns (ClearConversationRsp);
    rpc SelectByClientMsgId(SelectByClientMsgIdReq) returns (SelectByClientMsgIdRsp);
    rpc UpdateReadAck(UpdateReadAckReq) returns (UpdateReadAckRsp);
}

message SyncMessagesReq {
    string request_id = 1;
    string conversation_id = 2;
    uint64 after_seq = 3;
    int32 limit = 4;
}
message SyncMessagesRsp {
    ResponseHeader header = 1;
    repeated Message messages = 2;
    bool has_more = 3;
    uint64 latest_seq = 4;
}
// 其余 Req/Rsp 消息定义至少包含 request_id 和业务字段，响应包含 ResponseHeader
```

- [ ] **Step 4: 提交**

```bash
git add proto/message/
git commit -m "feat: 新增 message/ 域 proto 文件（types, service, internal）"
```

---

### Task 3: 创建 presence/ 域 proto 文件

**Files:**
- Create: `proto/presence/presence_service.proto`

- [ ] **Step 1: 创建 presence/presence_service.proto**

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

message SetPresenceReq {
    string request_id = 1;
    string user_id = 2;
    PresenceState state = 3;
    optional string custom_status = 4;
}
message SetPresenceRsp {
    ResponseHeader header = 1;
}

message GetPresenceReq {
    string request_id = 1;
    string user_id = 2;
}
message GetPresenceRsp {
    ResponseHeader header = 1;
    Presence presence = 2;
}

message BatchGetPresenceReq {
    string request_id = 1;
    repeated string user_ids = 2;
}
message BatchGetPresenceRsp {
    ResponseHeader header = 1;
    map<string, Presence> presences = 2;
}

message SubscribeReq {
    string request_id = 1;
    string user_id = 2;
    repeated string subscribe_user_ids = 3;
}
message SubscribeRsp { ResponseHeader header = 1; }

message UnsubscribeReq {
    string request_id = 1;
    string user_id = 2;
    repeated string unsubscribe_user_ids = 3;
}
message UnsubscribeRsp { ResponseHeader header = 1; }

message TypingReq {
    string request_id = 1;
    string user_id = 2;
    string conversation_id = 3;
    bool is_typing = 4;
}
message TypingRsp { ResponseHeader header = 1; }
```

- [ ] **Step 2: 提交**

```bash
git add proto/presence/presence_service.proto
git commit -m "feat: 新增 presence/ 域 proto 文件"
```

---

### Task 4: 新增 ODB 表 — message_reaction 和 message_pin

**Files:**
- Create: `odb/message_reaction.hxx`
- Create: `odb/message_pin.hxx`

- [ ] **Step 1: 创建 odb/message_reaction.hxx**

```cpp
#pragma once

#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

#pragma db object table("message_reaction")
class MessageReaction
{
public:
    MessageReaction() = default;
    MessageReaction(unsigned long mid, const std::string &uid, const std::string &e)
        : _message_id(mid), _user_id(uid), _emoji(e) {}

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string emoji() const { return _emoji; }
    void emoji(const std::string &v) { _emoji = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(16)")
    std::string _emoji;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db index("uk_msg_user_emoji") unique members(_message_id, _user_id, _emoji)
    #pragma db index("idx_msg") members(_message_id)
};

} // namespace chatnow
```

- [ ] **Step 2: 创建 odb/message_pin.hxx**

```cpp
#pragma once

#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

#pragma db object table("message_pin")
class MessagePin
{
public:
    MessagePin() = default;
    MessagePin(const std::string &ssid, unsigned long mid, const std::string &by)
        : _session_id(ssid), _message_id(mid), _pinned_by(by) {}

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &v) { _session_id = v; }

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string pinned_by() const { return _pinned_by; }
    void pinned_by(const std::string &v) { _pinned_by = v; }

    boost::posix_time::ptime pinned_at() const { return _pinned_at; }
    void pinned_at(const boost::posix_time::ptime &v) { _pinned_at = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _session_id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _pinned_by;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _pinned_at;

    #pragma db index("uk_conv_msg") unique members(_session_id, _message_id)
    #pragma db index("idx_conv") members(_session_id)
};

} // namespace chatnow
```

- [ ] **Step 3: 提交**

```bash
git add odb/message_reaction.hxx odb/message_pin.hxx
git commit -m "feat: 新增 ODB 表 message_reaction 和 message_pin"
```

---

### Task 5: 修改现有 ODB 表 — message、chat_session_member、chat_session

**Files:**
- Modify: `odb/message.hxx`
- Modify: `odb/chat_session_member.hxx`
- Modify: `odb/chat_session.hxx`

- [ ] **Step 1: 在 message.hxx 的 Message 类 private 段末尾（_revoke_by 之后）新增列**

在 `#pragma db type("varchar(32)")` + `odb::nullable<std::string> _revoke_by;` 之后添加：

```cpp
    // —— 编辑时间（proto Message.edited_at_ms） ——
    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _edit_time;

    // —— 转发来源（proto Message.forward_info） ——
    #pragma db type("varchar(32)")
    odb::nullable<std::string> _forward_from_uid;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _forward_at;
```

在 public 段 `revoke_by` 的 getter/setter 之后添加：

```cpp
    boost::posix_time::ptime edit_time() const {
        return _edit_time ? *_edit_time : boost::posix_time::ptime();
    }
    void edit_time(const boost::posix_time::ptime &v) { _edit_time = v; }

    std::string forward_from_uid() const {
        return _forward_from_uid ? *_forward_from_uid : std::string();
    }
    void forward_from_uid(const std::string &v) { _forward_from_uid = v; }

    boost::posix_time::ptime forward_at() const {
        return _forward_at ? *_forward_at : boost::posix_time::ptime();
    }
    void forward_at(const boost::posix_time::ptime &v) { _forward_at = v; }
```

- [ ] **Step 2: 在 chat_session_member.hxx 的 ChatSessionMember 类 private 段末尾（_quit_time 之后）新增 draft 列**

```cpp
    // —— 草稿（proto SelfMemberInfo.draft） ——
    #pragma db type("text")
    odb::nullable<std::string> _draft;
```

在 public 段 `quit_time` 的 getter/setter 之后添加：

```cpp
    std::string draft() const { return _draft ? *_draft : std::string(); }
    void draft(const std::string &v) { _draft = v; }
```

- [ ] **Step 3: 在 chat_session.hxx 的 ChatSessionType 枚举中新增 CHANNEL**

将：
```cpp
enum class ChatSessionType : unsigned char {
    SINGLE = 1,
    GROUP  = 2
};
```
改为：
```cpp
enum class ChatSessionType : unsigned char {
    SINGLE  = 1,
    GROUP   = 2,
    CHANNEL = 3    // 频道/广播（预留）
};
```

- [ ] **Step 4: 提交**

```bash
git add odb/message.hxx odb/chat_session_member.hxx odb/chat_session.hxx
git commit -m "feat: ODB 表加列——message(+edit_time/forward_*)、session_member(+draft)、session(+CHANNEL)"
```

---

### Task 6: 重构 identity/identity_service.proto（迁移 user.proto）

**Files:**
- Create: `proto/identity/identity_service.proto`
- Modify: `proto/user.proto`（暂保留，Phase 3 删除）

- [ ] **Step 1: 创建 proto/identity/identity_service.proto**

```protobuf
syntax = "proto3";
package chatnow.identity;

import "common/envelope.proto";
import "common/types.proto";

service IdentityService {
    rpc Register(RegisterReq) returns (RegisterRsp);
    rpc Login(LoginReq) returns (LoginRsp);
    rpc Logout(LogoutReq) returns (LogoutRsp);
    rpc SendVerifyCode(SendVerifyCodeReq) returns (SendVerifyCodeRsp);
    rpc RefreshToken(RefreshTokenReq) returns (RefreshTokenRsp);
    rpc GetProfile(GetProfileReq) returns (GetProfileRsp);
    rpc UpdateProfile(UpdateProfileReq) returns (UpdateProfileRsp);
    rpc SearchUsers(SearchUsersReq) returns (SearchUsersRsp);
    rpc GetMultiUserInfo(GetMultiUserInfoReq) returns (GetMultiUserInfoRsp);
}

message UsernamePassword {
    string username = 1;
    string password = 2;
}

message PhoneVerifyCode {
    string phone = 1;
    string verify_code_id = 2;
    string verify_code = 3;
}

message RegisterReq {
    string request_id = 1;
    oneof credential {
        UsernamePassword username_pwd = 2;
        PhoneVerifyCode phone_code = 3;
    }
    string nickname = 4;
}
message RegisterRsp {
    ResponseHeader header = 1;
    string user_id = 2;
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
message LoginRsp {
    ResponseHeader header = 1;
    string login_session_id = 2;
    UserInfo user_info = 3;
}

message LogoutReq {
    string request_id = 1;
    string user_id = 2;
    string session_id = 3;
}
message LogoutRsp { ResponseHeader header = 1; }

message SendVerifyCodeReq {
    string request_id = 1;
    string phone = 2;
}
message SendVerifyCodeRsp {
    ResponseHeader header = 1;
    string verify_code_id = 2;
}

message RefreshTokenReq {
    string request_id = 1;
    string session_id = 2;
}
message RefreshTokenRsp {
    ResponseHeader header = 1;
    string new_session_id = 2;
}

message GetProfileReq {
    string request_id = 1;
    optional string user_id = 2;      // 空=查自己
    optional string session_id = 3;
}
message GetProfileRsp {
    ResponseHeader header = 1;
    UserInfo user_info = 2;
}

message UpdateProfileReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    optional string nickname = 4;
    optional string bio = 5;
    optional string avatar_url = 6;
    optional string phone = 7;
}
message UpdateProfileRsp {
    ResponseHeader header = 1;
    UserInfo user_info = 2;
}

message SearchUsersReq {
    string request_id = 1;
    string search_key = 2;
    optional string session_id = 3;
    optional string user_id = 4;
}
message SearchUsersRsp {
    ResponseHeader header = 1;
    repeated UserInfo user_info = 2;
}

message GetMultiUserInfoReq {
    string request_id = 1;
    repeated string users_id = 2;
}
message GetMultiUserInfoRsp {
    ResponseHeader header = 1;
    map<string, UserInfo> users_info = 2;
}
```

- [ ] **Step 2: 提交**

```bash
git add proto/identity/identity_service.proto
git commit -m "feat: 新增 identity/identity_service.proto（迁移 user.proto）"
```

---

### Task 7: 重构 relationship/relationship_service.proto（迁移 friend.proto）

**Files:**
- Create: `proto/relationship/relationship_service.proto`

- [ ] **Step 1: 创建 proto/relationship/relationship_service.proto**

```protobuf
syntax = "proto3";
package chatnow.relationship;

import "common/envelope.proto";
import "common/types.proto";

service RelationshipService {
    rpc ListFriends(ListFriendsReq) returns (ListFriendsRsp);
    rpc SendFriendRequest(SendFriendReq) returns (SendFriendRsp);
    rpc HandleFriendRequest(HandleFriendReq) returns (HandleFriendRsp);
    rpc RemoveFriend(RemoveFriendReq) returns (RemoveFriendRsp);
    rpc SearchFriends(SearchFriendsReq) returns (SearchFriendsRsp);
    rpc BlockUser(BlockUserReq) returns (BlockUserRsp);
    rpc UnblockUser(UnblockUserReq) returns (UnblockUserRsp);
    rpc ListBlockedUsers(ListBlockedReq) returns (ListBlockedRsp);
    rpc ListPendingRequests(ListPendingReq) returns (ListPendingRsp);
}

message ListFriendsReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    PageRequest page = 4;
}
message ListFriendsRsp {
    ResponseHeader header = 1;
    repeated UserInfo friend_list = 2;
    PageResponse page = 3;
}

message SendFriendReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string respondent_id = 4;
}
message SendFriendRsp {
    ResponseHeader header = 1;
    optional string notify_event_id = 2;
}

message HandleFriendReq {
    string request_id = 1;
    string notify_event_id = 2;
    bool agree = 3;
    string apply_user_id = 4;
    optional string session_id = 5;
    optional string user_id = 6;
}
message HandleFriendRsp {
    ResponseHeader header = 1;
    optional string new_conversation_id = 2;
}

message RemoveFriendReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    string peer_id = 4;
}
message RemoveFriendRsp { ResponseHeader header = 1; }

message SearchFriendsReq {
    string request_id = 1;
    string search_key = 2;
    optional string session_id = 3;
    optional string user_id = 4;
}
message SearchFriendsRsp {
    ResponseHeader header = 1;
    repeated UserInfo user_info = 2;
}

message BlockUserReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    string peer_id = 4;
}
message BlockUserRsp { ResponseHeader header = 1; }

message UnblockUserReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    string peer_id = 4;
}
message UnblockUserRsp { ResponseHeader header = 1; }

message ListBlockedReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    PageRequest page = 4;
}
message ListBlockedRsp {
    ResponseHeader header = 1;
    repeated UserInfo blocked_list = 2;
    PageResponse page = 3;
}

message ListPendingReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
}
message ListPendingRsp {
    ResponseHeader header = 1;
    repeated FriendEvent event = 2;
}

message FriendEvent {
    optional string event_id = 1;
    UserInfo sender = 2;
}
```

- [ ] **Step 2: 提交**

```bash
git add proto/relationship/relationship_service.proto
git commit -m "feat: 新增 relationship/relationship_service.proto（迁移 friend.proto，加黑名单）"
```

---

### Task 8: 重构 conversation/conversation_service.proto（迁移 chatsession.proto）

**Files:**
- Create: `proto/conversation/conversation_service.proto`

- [ ] **Step 1: 创建 proto/conversation/conversation_service.proto**

```protobuf
syntax = "proto3";
package chatnow.conversation;

import "common/envelope.proto";
import "common/types.proto";
import "message/message_types.proto";  // 仅引用 MessagePreview

enum ConversationType {
    CONVERSATION_TYPE_UNSPECIFIED = 0;
    PRIVATE = 1;
    GROUP = 2;
    CHANNEL = 3;
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
    optional SelfMemberInfo self = 11;
}

message SelfMemberInfo {
    MemberRole role = 1;
    int64 joined_at_ms = 2;
    bool is_muted = 3;
    bool is_pinned = 4;
    int64 pin_time_ms = 5;
    bool is_visible = 6;
    uint64 last_read_seq = 7;
    uint64 unread_count = 8;
    optional string draft = 9;
}

message MemberItem {
    UserInfo user_info = 1;
    MemberRole role = 2;
    int64 join_time_ms = 3;
}

service ConversationService {
    rpc ListConversations(ListConversationsReq) returns (ListConversationsRsp);
    rpc GetConversation(GetConversationReq) returns (GetConversationRsp);
    rpc CreateConversation(CreateConversationReq) returns (CreateConversationRsp);
    rpc UpdateConversation(UpdateConversationReq) returns (UpdateConversationRsp);
    rpc DismissConversation(DismissConversationReq) returns (DismissConversationRsp);
    rpc AddMembers(AddMembersReq) returns (AddMembersRsp);
    rpc RemoveMembers(RemoveMembersReq) returns (RemoveMembersRsp);
    rpc TransferOwner(TransferOwnerReq) returns (TransferOwnerRsp);
    rpc ChangeMemberRole(ChangeMemberRoleReq) returns (ChangeMemberRoleRsp);
    rpc ListMembers(ListMembersReq) returns (ListMembersRsp);
    rpc SetMute(SetMuteReq) returns (SetMuteRsp);
    rpc SetPin(SetPinReq) returns (SetPinRsp);
    rpc SetVisible(SetVisibleReq) returns (SetVisibleRsp);
    rpc QuitConversation(QuitConversationReq) returns (QuitConversationRsp);
    rpc MarkRead(MarkReadReq) returns (MarkReadRsp);
    rpc SaveDraft(SaveDraftReq) returns (SaveDraftRsp);
    rpc SearchConversations(SearchConversationsReq) returns (SearchConversationsRsp);
    rpc GetMemberIds(GetMemberIdsReq) returns (GetMemberIdsRsp);
}

// 列表
message ListConversationsReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    PageRequest page = 4;
}
message ListConversationsRsp {
    ResponseHeader header = 1;
    repeated Conversation conversations = 2;
    PageResponse page = 3;
}

// 详情
message GetConversationReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
}
message GetConversationRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

// 创建
message CreateConversationReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string name = 4;
    repeated string member_id_list = 5;
}
message CreateConversationRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

// 更新
message UpdateConversationReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    optional string name = 5;
    optional string avatar_url = 6;
    optional string description = 7;
}
message UpdateConversationRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

// 解散
message DismissConversationReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
}
message DismissConversationRsp { ResponseHeader header = 1; }

// 成员管理
message AddMembersReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    repeated string member_id_list = 5;
}
message AddMembersRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

message RemoveMembersReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    repeated string member_id_list = 5;
}
message RemoveMembersRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

message TransferOwnerReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    string new_owner_id = 5;
}
message TransferOwnerRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

message ChangeMemberRoleReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    string changed_user_id = 5;
    MemberRole role = 6;
}
message ChangeMemberRoleRsp {
    ResponseHeader header = 1;
    Conversation conversation = 2;
}

message ListMembersReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    PageRequest page = 5;
}
message ListMembersRsp {
    ResponseHeader header = 1;
    repeated MemberItem members = 2;
    PageResponse page = 3;
}

// 个人设置
message SetMuteReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    bool is_muted = 5;
}
message SetMuteRsp {
    ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message SetPinReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    bool is_pinned = 5;
}
message SetPinRsp {
    ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message SetVisibleReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    bool is_visible = 5;
}
message SetVisibleRsp {
    ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message QuitConversationReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
}
message QuitConversationRsp { ResponseHeader header = 1; }

// 已读 + 草稿
message MarkReadReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    uint64 seq_id = 5;
}
message MarkReadRsp { ResponseHeader header = 1; }

message SaveDraftReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
    string draft = 5;
}
message SaveDraftRsp { ResponseHeader header = 1; }

// 搜索
message SearchConversationsReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string search_key = 4;
    optional ConversationType type = 5;
}
message SearchConversationsRsp {
    ResponseHeader header = 1;
    repeated Conversation conversations = 2;
}

// 内部
message GetMemberIdsReq {
    string request_id = 1;
    optional string session_id = 2;
    optional string user_id = 3;
    string conversation_id = 4;
}
message GetMemberIdsRsp {
    ResponseHeader header = 1;
    repeated string member_id_list = 2;
}
```

- [ ] **Step 2: 提交**

```bash
git add proto/conversation/conversation_service.proto
git commit -m "feat: 新增 conversation/conversation_service.proto（迁移 chatsession.proto）"
```

---

### Task 9: 重构 media/media_service.proto 和 push/ 域

**Files:**
- Create: `proto/media/media_service.proto`
- Create: `proto/push/push_service.proto`
- Create: `proto/push/notify.proto`
- Create: `proto/transmite/transmite_service.proto`

- [ ] **Step 1: 创建 proto/media/media_service.proto**

合并 `file.proto` + `speech.proto`：

```protobuf
syntax = "proto3";
package chatnow.media;

import "common/envelope.proto";

message FileDownloadData {
    string file_id = 1;
    bytes file_content = 2;
}

message FileUploadData {
    string file_name = 1;
    int64 file_size = 2;
    bytes file_content = 3;
}

message FileInfo {
    string file_id = 1;
    string file_name = 2;
    int64 file_size = 3;
}

service MediaService {
    rpc UploadFile(UploadFileReq) returns (UploadFileRsp);
    rpc DownloadFile(DownloadFileReq) returns (DownloadFileRsp);
    rpc GetFileInfo(GetFileInfoReq) returns (GetFileInfoRsp);
    rpc SpeechRecognition(SpeechRecognitionReq) returns (SpeechRecognitionRsp);
}

message UploadFileReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    FileUploadData file_data = 4;
}
message UploadFileRsp {
    ResponseHeader header = 1;
    FileInfo file_info = 2;
}

message DownloadFileReq {
    string request_id = 1;
    string file_id = 2;
    optional string user_id = 3;
    optional string session_id = 4;
}
message DownloadFileRsp {
    ResponseHeader header = 1;
    FileDownloadData file_data = 2;
}

message GetFileInfoReq {
    string request_id = 1;
    string file_id = 2;
    optional string user_id = 3;
    optional string session_id = 4;
}
message GetFileInfoRsp {
    ResponseHeader header = 1;
    FileInfo file_info = 2;
}

message SpeechRecognitionReq {
    string request_id = 1;
    bytes speech_content = 2;
    optional string user_id = 3;
    optional string session_id = 4;
}
message SpeechRecognitionRsp {
    ResponseHeader header = 1;
    string recognition_result = 2;
}
```

- [ ] **Step 2: 创建 proto/push/push_service.proto**

保持原 push.proto 结构，添加 `chatnow.common.ResponseHeader` import：

```protobuf
syntax = "proto3";
package chatnow.push;

import "common/envelope.proto";

message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    bytes notify_payload = 3;
    optional uint64 user_seq = 4;
}
message PushToUserRsp {
    ResponseHeader header = 1;
    int32 online_device_count = 2;
}

message PushBatchReq {
    string request_id = 1;
    repeated string user_id_list = 2;
    bytes notify_payload = 3;
    repeated UserSeqPair user_seqs = 4;
}
message PushBatchRsp {
    ResponseHeader header = 1;
    int32 online_count = 2;
}

message UserSeqPair {
    string user_id = 1;
    uint64 user_seq = 2;
}

service PushService {
    rpc PushToUser(PushToUserReq) returns (PushToUserRsp);
    rpc PushBatch(PushBatchReq) returns (PushBatchRsp);
}
```

- [ ] **Step 3: 创建 proto/push/notify.proto**

```protobuf
syntax = "proto3";
package chatnow.push;

import "common/types.proto";
import "message/message_types.proto";

enum NotifyType {
    FRIEND_ADD_APPLY_NOTIFY = 0;
    FRIEND_ADD_PROCESS_NOTIFY = 1;
    CONVERSATION_CREATE_NOTIFY = 2;
    CHAT_MESSAGE_NOTIFY = 3;
    FRIEND_REMOVE_NOTIFY = 4;
    MESSAGE_RECALLED_NOTIFY = 5;     // 新增：撤回通知
    PRESENCE_CHANGE_NOTIFY = 6;      // 新增：状态变化
    TYPING_NOTIFY = 7;               // 新增：输入中
    CLIENT_AUTH = 49;
    MSG_PUSH_ACK = 50;
    CLIENT_HEARTBEAT = 51;
}

message NotifyClientAuth {
    string session_id = 1;
    string device_id = 2;
    optional uint64 last_user_seq = 3;
}

message NotifyMsgPushAck {
    string user_id = 1;
    int64 message_id = 2;
    uint64 user_seq = 3;
    string conversation_id = 4;
}

message NotifyHeartbeat {
    string user_id = 1;
    uint64 last_user_seq = 2;
}

message NotifyFriendAddApply { UserInfo user_info = 1; }
message NotifyFriendAddProcess { bool agree = 1; UserInfo user_info = 2; }
message NotifyFriendRemove { string user_id = 1; }
message NotifyNewConversation { bytes conversation_payload = 1; }
message NotifyNewMessage { Message message_info = 1; }
message NotifyMessageRecalled {
    string conversation_id = 1;
    int64 message_id = 2;
}
message NotifyPresenceChange {
    string user_id = 1;
    string state = 2;
}
message NotifyTyping {
    string user_id = 1;
    string conversation_id = 2;
    bool is_typing = 3;
}

message NotifyMessage {
    optional string notify_event_id = 1;
    NotifyType notify_type = 2;
    oneof notify_remarks {
        NotifyFriendAddApply friend_add_apply = 3;
        NotifyFriendAddProcess friend_process_result = 4;
        NotifyFriendRemove friend_remove = 7;
        NotifyNewConversation new_conversation_info = 5;
        NotifyNewMessage new_message_info = 6;
        NotifyMsgPushAck msg_push_ack = 8;
        NotifyHeartbeat heartbeat = 9;
        NotifyClientAuth client_auth = 10;
        NotifyMessageRecalled message_recalled = 11;
        NotifyPresenceChange presence_change = 12;
        NotifyTyping typing = 13;
    }
}
```

- [ ] **Step 4: 创建 proto/transmite/transmite_service.proto**

```protobuf
syntax = "proto3";
package chatnow.transmite;

import "common/envelope.proto";
import "message/message_types.proto";

message NewMessageReq {
    string request_id = 1;
    optional string user_id = 2;
    optional string session_id = 3;
    string conversation_id = 4;
    MessageContent content = 5;
    string client_msg_id = 6;
}
message NewMessageRsp {
    ResponseHeader header = 1;
    int64 message_id = 2;
    uint64 seq_id = 3;
}

message GetTransmitTargetRsp {
    ResponseHeader header = 1;
    Message message = 2;
    repeated string target_id_list = 3;
}

service MsgTransmitService {
    rpc GetTransmitTarget(NewMessageReq) returns (GetTransmitTargetRsp);
}
```

- [ ] **Step 5: 提交**

```bash
git add proto/media/ proto/push/ proto/transmite/
git commit -m "feat: 新增 media/、push/、transmite/ 域 proto 文件"
```

---

### Task 10: MessageType 枚举对齐 + Presence Redis Key

**Files:**
- Modify: `odb/message.hxx`（枚举值调整）
- Modify: `common/dao/data_redis.hpp`（新增 Presence key + Presence Redis 操作类）

- [ ] **Step 1: 对齐 message.hxx 的 MessageType 枚举**

在 `odb/message.hxx` 中将：
```cpp
enum class MessageType : unsigned char {
    UNKNOWN  = 0,
    TEXT     = 1,
    IMAGE    = 2,
    FILE     = 3,
    SPEECH   = 4,
    VIDEO    = 5,
    LOCATION = 6,
    CARD     = 7,
    SYSTEM   = 99
};
```
改为：
```cpp
enum class MessageType : unsigned char {
    UNKNOWN       = 0,
    TEXT          = 1,
    IMAGE         = 2,
    FILE          = 3,
    AUDIO         = 4,   // 原名 SPEECH，值不变，兼容历史数据
    VIDEO         = 5,
    LOCATION      = 6,
    STICKER       = 7,   // 原名 CARD，值 7 不变
    SYSTEM_NOTICE = 8    // 原名 SYSTEM=99 迁移至此
};
```

- [ ] **Step 2: 在 data_redis.hpp 的 namespace key 中新增 Presence Redis key 常量**

在 `namespace key` 末尾（`}` 之前）新增：

```cpp
    // --- Presence 域（Push 内模块） ---
    inline constexpr const char* kPresence        = "im:presence:";         // {uid} → HASH {state,last_active,custom_status}
    inline constexpr const char* kPresenceDevices = "im:presence:devices:"; // {uid} → SET<device_id>, TTL 120s
    inline constexpr const char* kPresenceTyping  = "im:presence:typing:";  // {uid} → SET<conversation_id>, TTL 10s
    inline constexpr const char* kPresenceSub     = "im:presence:sub:";     // {uid} → SET<user_id>
```

- [ ] **Step 3: 在 data_redis.hpp 末尾新增 PresenceRedis 操作类**

在 ESOutbox 类之后添加：

```cpp
/**
 * PresenceRedis — Presence 状态的 Redis 操作封装
 * 运行时在 Push 进程内调用，无 RPC 开销
 */
class PresenceRedis
{
public:
    using ptr = std::shared_ptr<PresenceRedis>;
    PresenceRedis(const std::shared_ptr<sw::redis::Redis> &r) : _r(r) {}

    /* 设置状态 */
    void set_state(const std::string &uid, const std::string &state) {
        _r->hset(key::kPresence + uid, "state", state);
    }

    /* 获取状态 */
    std::string get_state(const std::string &uid) {
        auto v = _r->hget(key::kPresence + uid, "state");
        return v ? *v : "offline";
    }

    /* 更新最后活跃时间 */
    void touch_active(const std::string &uid, int64_t ts_ms) {
        _r->hset(key::kPresence + uid, "last_active", std::to_string(ts_ms));
    }

    /* 设置自定义状态 */
    void set_custom_status(const std::string &uid, const std::string &text) {
        _r->hset(key::kPresence + uid, "custom_status", text);
    }

    /* 添加在线设备（心跳续期） */
    void add_device(const std::string &uid, const std::string &device_id) {
        auto k = key::kPresenceDevices + uid;
        _r->sadd(k, device_id);
        _r->expire(k, std::chrono::seconds(120));
    }

    /* 获取在线设备列表 */
    std::vector<std::string> get_devices(const std::string &uid) {
        std::vector<std::string> out;
        _r->smembers(key::kPresenceDevices + uid, std::back_inserter(out));
        return out;
    }

    /* 输入中指示 */
    void set_typing(const std::string &uid, const std::string &conv_id) {
        auto k = key::kPresenceTyping + uid;
        _r->sadd(k, conv_id);
        _r->expire(k, std::chrono::seconds(10));
    }

    /* 订阅状态 */
    void subscribe(const std::string &uid, const std::string &target_uid) {
        _r->sadd(key::kPresenceSub + uid, target_uid);
    }

    void unsubscribe(const std::string &uid, const std::string &target_uid) {
        _r->srem(key::kPresenceSub + uid, target_uid);
    }

private:
    std::shared_ptr<sw::redis::Redis> _r;
};
```

- [ ] **Step 4: 提交**

```bash
git add odb/message.hxx common/dao/data_redis.hpp
git commit -m "feat: MessageType 枚举对齐 + Presence Redis key 和操作类"
```

---

### Task 11: 清理旧 proto 文件 + 更新 CMakeLists.txt

**Files:**
- Delete: `proto/base.proto`, `user.proto`, `friend.proto`, `chatsession.proto`, `file.proto`, `speech.proto`, `gateway.proto`
- Modify: `CMakeLists.txt`（或 proto 编译脚本）

- [ ] **Step 1: 更新 CMakeLists.txt 的 proto 编译目标**

找到 `CMakeLists.txt` 中编译 proto 的部分（通常是 `protobuf_generate_cpp` 或类似），将旧的 proto 文件路径替换为新的：

```
# 旧（删除）:
#   proto/base.proto
#   proto/user.proto
#   proto/friend.proto
#   proto/chatsession.proto
#   proto/file.proto
#   proto/speech.proto
#   proto/gateway.proto

# 新（保留 + 新增）:
#   proto/common/types.proto
#   proto/common/error.proto
#   proto/common/envelope.proto
#   proto/identity/identity_service.proto
#   proto/relationship/relationship_service.proto
#   proto/conversation/conversation_service.proto
#   proto/message/message_types.proto
#   proto/message/message_service.proto
#   proto/message/message_internal.proto
#   proto/presence/presence_service.proto
#   proto/push/push_service.proto
#   proto/push/notify.proto
#   proto/media/media_service.proto
#   proto/transmite/transmite_service.proto
```

- [ ] **Step 2: 删除旧 proto 文件**

```bash
git rm proto/base.proto proto/user.proto proto/friend.proto \
       proto/chatsession.proto proto/file.proto proto/speech.proto \
       proto/gateway.proto
```

- [ ] **Step 3: 验证编译通过**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -20
# Expected: 编译成功，无 proto 相关错误
```

- [ ] **Step 4: 提交**

```bash
git add CMakeLists.txt
git commit -m "refactor: 清理旧 proto 文件 + 更新 CMakeLists.txt 编译目标"
```

---

### Task 12: 更新所有 C++ 代码的 proto include 路径 + 最终构建验证

**Files:**
- Modify: `*_server.h`、`*_server.cc` 中所有 `#include "base.pb.h"` → 新的 domain proto 头文件
- Modify: DAO 文件中 proto→ODB 映射层（如有硬编码的 proto 字段名）

- [ ] **Step 1: 列出所有需要更新 include 的文件**

```bash
cd /Users/yanghaoyang/repo/ChatNow
grep -rn 'base.pb.h\|user.pb.h\|friend.pb.h\|chatsession.pb.h\|file.pb.h\|speech.pb.h\|gateway.pb.h\|notify.pb.h\|push.pb.h\|transmite.pb.h\|message.pb.h' --include="*.h" --include="*.cc" --include="*.hpp" .
```

- [ ] **Step 2: 批量更新 include 路径**

对每个文件，将旧 proto 头文件引用替换为新路径。例如：

```
旧: #include "base.pb.h"
新: #include "common/types.pb.h" + #include "common/envelope.pb.h"

旧: #include "user.pb.h"
新: #include "identity/identity_service.pb.h"

旧: #include "friend.pb.h"
新: #include "relationship/relationship_service.pb.h"

旧: #include "chatsession.pb.h"
新: #include "conversation/conversation_service.pb.h"

旧: #include "message.pb.h"
新: #include "message/message_service.pb.h" + #include "message/message_types.pb.h"

旧: #include "file.pb.h" 或 "speech.pb.h"
新: #include "media/media_service.pb.h"

旧: #include "push.pb.h" 或 "notify.pb.h"
新: #include "push/push_service.pb.h" + #include "push/notify.pb.h"

旧: #include "transmite.pb.h"
新: #include "transmite/transmite_service.pb.h"
```

- [ ] **Step 3: 更新 proto namespace 引用**

proto 文件 package 名从 `chatnow` 改为分域（如 `chatnow.identity`、`chatnow.message`），C++ 生成的 namespace 对应变化：

```
旧: chatnow::UserInfo
新: chatnow::common::UserInfo

旧: chatnow::MessageInfo
新: chatnow::message::Message

旧: chatnow::ChatSessionInfo
新: chatnow::conversation::Conversation
```

- [ ] **Step 4: DAO 映射层字段名适配**

DAO 中将 DB 列映射到 proto 字段时，字段名需要与新的 proto 定义对齐。例如 `chat_session_id()` → `conversation_id()`，`user_id()` → `sender_id()`（仅 proto 侧，DB 列名不变）。

- [ ] **Step 5: 全量构建验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build
make -j$(nproc) 2>&1 | tail -30
# Expected: 编译成功，0 errors
```

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "refactor: 更新所有 C++ 代码的 proto include 路径和 namespace 引用"
```

---

## 任务依赖关系

```
Task 1 (common/) ─────┬──→ Task 2 (message/) ──→ Task 8 (conversation/) ─┐
                      │                                                    │
                      ├──→ Task 3 (presence/)                              │
                      │                                                    │
                      ├──→ Task 6 (identity/)                              │
                      │                                                    │
                      ├──→ Task 7 (relationship/)                          │
                      │                                                    │
                      └──→ Task 9 (media/push/transmite/)                  │
                                                                           │
Task 4 (DB 新表) ─────────────────────────────────────────────────────────┤
                                                                           │
Task 5 (DB 改表) ──→ Task 10 (枚举对齐+Redis) ────────────────────────────┤
                                                                           │
                                                                           ▼
                                                                   Task 11 (清理)
                                                                        │
                                                                        ▼
                                                                   Task 12 (include 更新)
```

Phase 1（Task 1-5）：并行执行，互不依赖
Phase 2（Task 6-10）：依赖 Phase 1 的 common/proto，但相互独立可并行
Phase 3（Task 11-12）：依赖 Phase 2 全部完成

---

## 预计变更量

| 阶段 | 新增文件 | 修改文件 | 删除文件 | 预计代码行 |
|------|---------|---------|---------|-----------|
| Phase 1 | 7 proto + 2 ODB | 0 | 0 | ~600 |
| Phase 2 | 6 proto + Redis | 2 ODB | 0 | ~700 |
| Phase 3 | 0 | ~15 C++ 文件 | 7 proto | ~200 |
| **合计** | **15** | **~17** | **7** | **~1500** |
