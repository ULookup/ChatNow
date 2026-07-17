# Conversation Service Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `chatsession/source/` 下的旧 `ChatSessionServiceImpl : public ChatSessionService` 迁到新 `chatnow::conversation::ConversationServiceImpl`，目录改名 `chatsession/` → `conversation/`，ODB 实体 / 表名 / DAO 类名同步改名。18 个 RPC 全部用新 proto + `HANDLE_RPC` 宏 + metadata 鉴权（含 4 个新增：DismissConversation / SaveDraft / MarkRead / GetUnreadCount 合并入 SelfMemberInfo）。Gateway 16 个 chatsession HTTP handler 跟进切到新 stub。Message 服务 stub（`SyncMessages`）按预期写新名字，编译阻塞与 Relationship 当前同质，等 Message 迁移期一起过。

**Architecture:** 沿用既有"Server / ServiceImpl / Builder + main.cc"分层。`HANDLE_RPC` 宏统一处理 `extract_auth + LogContext + ResponseHeader + ServiceError`。跨服务调用前 `forward_auth_metadata` 透传 4 个鉴权 metadata。ODB 实体 `ChatSession` → `Conversation`、`ChatSessionMember` → `ConversationMember`、`OrderedChatSessionView` → `OrderedConversationView`；表名 `chat_session` → `conversation`、`chat_session_member` → `conversation_member`；ES 索引名字符串字面量保留 `"chat_session"`（YAGNI 不重建索引）。Redis key 前缀 `chatsession:members:` → `conversation:members:`（开发阶段直接改）。

**Tech Stack:** C++17 / brpc + Protobuf 3 / ODB MySQL / Elasticsearch (elasticlient) / etcd-cpp-api / spdlog / boost::posix_time / GoogleTest。

---

## 文件结构

**新增**：
- `odb/conversation.hxx` — `Conversation` 实体（替代 `chat_session.hxx`），表名 `conversation`
- `odb/conversation_member.hxx` — `ConversationMember` 实体（替代 `chat_session_member.hxx`），表名 `conversation_member`
- `odb/conversation_view.hxx` — `OrderedConversationView`（替代 `chat_session_view.hxx`）
- `common/dao/mysql_conversation.hpp` — `ConversationTable` DAO
- `common/dao/mysql_conversation_member.hpp` — `ConversationMemberTable` DAO（含 4 个新方法）
- `conversation/CMakeLists.txt` — 编译 target `conversation_server`
- `conversation/Dockerfile` — 镜像
- `conversation/source/conversation_server.h` — `ConversationServiceImpl` + `ConversationServer` + `ConversationServerBuilder`
- `conversation/source/conversation_server.cc` — main 入口（gflags + builder）
- `conf/conversation_server.conf` — gflags flagfile

**删除**：
- `chatsession/CMakeLists.txt`、`chatsession/source/chatsession_server.h`、`chatsession/source/chatsession_server.cc`、`conf/chatsession_server.conf`
- `odb/chat_session.hxx`、`odb/chat_session_member.hxx`、`odb/chat_session_view.hxx`
- `common/dao/mysql_chat_session.hpp`、`common/dao/mysql_chat_session_member.hpp`
- `proto/chatsession.proto`（最后一步，全仓零引用后再删）

**修改**：
- `proto/conversation/conversation_service.proto` — 删 36 处 `optional user_id / session_id`；加 `option cc_generic_services = true;`；引用全限定到 `chatnow.common.*` / `chatnow.identity.UserInfo` / `chatnow.message.MessagePreview`
- `common/dao/data_es.hpp` — `ESChatSession` 类改名 `ESConversation`，内部 index 字符串字面量保留 `"chat_session"`
- `common/dao/data_redis.hpp` — `kMembers` Redis key 前缀 `im:members:` → `im:conversation:members:`（语义对齐）
- `CMakeLists.txt`（顶层）— `add_subdirectory(chatsession)` → `add_subdirectory(conversation)`
- `gateway/source/gateway_server.h` — 16 个 chatsession handler 切到新 stub 与新 RPC 名（HTTP 路径不变）；同时修正 spec §11.6.3 提到的 3 处 `_relationship_service_name` 误用为 `_chatsession_service_name` 的 pre-existing bug
- `docker-compose.yml` — `chatsession_server` service → `conversation_server`
- `transmite/source/transmite_server.h` — `ChatSessionService_Stub` → `chatnow::conversation::ConversationService_Stub`，`GetMemberIdList` → `GetMemberIds`（Transmite 服务尚未做正式迁移，但本期为了让 Conversation 服务起步后 Transmite 还能调到新接口，仅改 stub 名字与 RPC 方法名，不动其它逻辑）
- `message/source/message_server.h` — `ChatSessionMemberTable::ptr` 改名为 `ConversationMemberTable::ptr`（仅 typedef + include 路径）；不动业务（Message 完整迁移留给后续 spec）

---

## Task 1: 清理 conversation_service.proto 鉴权字段 + 全限定引用

**Files:**
- Modify: `proto/conversation/conversation_service.proto`

- [ ] **Step 1: 整文件替换 conversation_service.proto**

把 `proto/conversation/conversation_service.proto` 全文替换为下面内容（删 36 处 `optional user_id` / `optional session_id`，加 `option cc_generic_services`，引用全限定）：

```protobuf
syntax = "proto3";
package chatnow.conversation;

option cc_generic_services = true;

import "common/envelope.proto";
import "common/types.proto";
import "message/message_types.proto";

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
    optional chatnow.message.MessagePreview last_message = 10;
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
    chatnow.common.UserInfo user_info = 1;
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

message ListConversationsReq {
    string request_id = 1;
    chatnow.common.PageRequest page = 2;
}
message ListConversationsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated Conversation conversations = 2;
    chatnow.common.PageResponse page = 3;
}

message GetConversationReq {
    string request_id = 1;
    string conversation_id = 2;
}
message GetConversationRsp {
    chatnow.common.ResponseHeader header = 1;
    Conversation conversation = 2;
}

message CreateConversationReq {
    string request_id = 1;
    ConversationType type = 2;
    optional string name = 3;
    optional string avatar_url = 4;
    optional string description = 5;
    repeated string member_ids = 6;
}
message CreateConversationRsp {
    chatnow.common.ResponseHeader header = 1;
    Conversation conversation = 2;
}

message UpdateConversationReq {
    string request_id = 1;
    string conversation_id = 2;
    optional string name = 3;
    optional string avatar_url = 4;       // 实际承载 avatar_file_id；服务端转 URL
    optional string description = 5;
    optional string announcement = 6;
}
message UpdateConversationRsp {
    chatnow.common.ResponseHeader header = 1;
    Conversation conversation = 2;
}

message DismissConversationReq {
    string request_id = 1;
    string conversation_id = 2;
}
message DismissConversationRsp { chatnow.common.ResponseHeader header = 1; }

message AddMembersReq {
    string request_id = 1;
    string conversation_id = 2;
    repeated string member_ids = 3;
}
message AddMembersRsp { chatnow.common.ResponseHeader header = 1; }

message RemoveMembersReq {
    string request_id = 1;
    string conversation_id = 2;
    repeated string member_ids = 3;
}
message RemoveMembersRsp { chatnow.common.ResponseHeader header = 1; }

message TransferOwnerReq {
    string request_id = 1;
    string conversation_id = 2;
    string new_owner_id = 3;
}
message TransferOwnerRsp { chatnow.common.ResponseHeader header = 1; }

message ChangeMemberRoleReq {
    string request_id = 1;
    string conversation_id = 2;
    string target_user_id = 3;
    MemberRole role = 4;
}
message ChangeMemberRoleRsp { chatnow.common.ResponseHeader header = 1; }

message ListMembersReq {
    string request_id = 1;
    string conversation_id = 2;
    chatnow.common.PageRequest page = 3;
}
message ListMembersRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated MemberItem members = 2;
    chatnow.common.PageResponse page = 3;
}

message SetMuteReq {
    string request_id = 1;
    string conversation_id = 2;
    bool mute = 3;
}
message SetMuteRsp {
    chatnow.common.ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message SetPinReq {
    string request_id = 1;
    string conversation_id = 2;
    bool pin = 3;
}
message SetPinRsp {
    chatnow.common.ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message SetVisibleReq {
    string request_id = 1;
    string conversation_id = 2;
    bool visible = 3;
}
message SetVisibleRsp {
    chatnow.common.ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message QuitConversationReq {
    string request_id = 1;
    string conversation_id = 2;
}
message QuitConversationRsp { chatnow.common.ResponseHeader header = 1; }

message MarkReadReq {
    string request_id = 1;
    string conversation_id = 2;
    uint64 last_read_seq = 3;
}
message MarkReadRsp { chatnow.common.ResponseHeader header = 1; }

message SaveDraftReq {
    string request_id = 1;
    string conversation_id = 2;
    string draft = 3;
}
message SaveDraftRsp {
    chatnow.common.ResponseHeader header = 1;
    SelfMemberInfo self = 2;
}

message SearchConversationsReq {
    string request_id = 1;
    string search_key = 2;
}
message SearchConversationsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated Conversation conversations = 2;
}

message GetMemberIdsReq {
    string request_id = 1;
    string conversation_id = 2;
}
message GetMemberIdsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated string member_ids = 2;
}
```

- [ ] **Step 2: 验证 grep 零命中**

Run: `grep -nE "optional string user_id|optional string session_id" proto/conversation/conversation_service.proto`
Expected: 0 lines

Run: `grep -n "cc_generic_services" proto/conversation/conversation_service.proto`
Expected: `option cc_generic_services = true;` 命中 1 行

- [ ] **Step 3: Commit**

```bash
git add proto/conversation/conversation_service.proto
git commit -m "$(cat <<'EOF'
proto(conversation): 删鉴权字段 + 全限定 + cc_generic_services

删 36 处 optional user_id/session_id，加 option cc_generic_services=true，
引用全限定到 chatnow.common.* / chatnow.message.MessagePreview /
chatnow.identity.UserInfo（间接通过 common UserInfo 实际定义）。
EOF
)"
```

> 注：proto 中 `MemberItem.user_info` 字段类型用 `chatnow.common.UserInfo`，因为 `common/types.proto` 已经定义了 UserInfo（与 Identity 共享）；不引用 `chatnow.identity` 包的本地拷贝。如发现 `common/types.proto` 没有 UserInfo，则改为 `import "identity/identity_service.proto"; chatnow.identity.UserInfo`，并把上面 proto 全文 user_info 类型同步改。Step 4 用于校验。

- [ ] **Step 4: 校验 UserInfo 定义位置**

Run: `grep -n "message UserInfo" proto/common/types.proto proto/identity/identity_service.proto`
Expected: 至少一行命中。如果只在 `identity/identity_service.proto` 命中，回到 Step 1 把 proto 中 `MemberItem.user_info` 改为 `chatnow.identity.UserInfo`，并加 `import "identity/identity_service.proto";`，重新提交修订。

---

## Task 2: 新建 ODB Conversation 实体（替代 chat_session.hxx）

**Files:**
- Create: `odb/conversation.hxx`

- [ ] **Step 1: 写 odb/conversation.hxx**

```cpp
#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * 会话主表 (conversation) — 替代旧 chat_session.hxx
 * 字段集合不变，仅类名/枚举名/表名 rename：
 *   ChatSession         → Conversation
 *   ChatSessionType     → ConversationType   (PRIVATE=1, GROUP=2, CHANNEL=3)
 *   ChatSessionStatus   → ConversationStatus (NORMAL/ARCHIVED/DISMISSED/BANNED)
 *   table chat_session  → table conversation
 *   _chat_session_id    → _conversation_id
 *   _chat_session_name  → _conversation_name
 *   _chat_session_type  → _conversation_type
 */

namespace chatnow
{

enum class ConversationType : unsigned char {
    PRIVATE = 1,
    GROUP   = 2,
    CHANNEL = 3
};

enum class ConversationStatus : unsigned char {
    NORMAL    = 0,
    ARCHIVED  = 1,
    DISMISSED = 2,
    BANNED    = 3
};

#pragma db object table("conversation")
class Conversation
{
public:
    Conversation() = default;
    Conversation(const std::string &cid,
                 const std::string &cname,
                 ConversationType ctype,
                 const boost::posix_time::ptime &create_time,
                 int member_count,
                 ConversationStatus status)
        : _conversation_id(cid), _conversation_name(cname),
          _conversation_type(ctype), _create_time(create_time),
          _member_count(member_count), _status(status) {}

    std::string conversation_id() const { return _conversation_id; }
    void conversation_id(const std::string &v) { _conversation_id = v; }

    std::string conversation_name() const { return _conversation_name ? *_conversation_name : std::string(); }
    void conversation_name(const std::string &v) { _conversation_name = v; }

    ConversationType conversation_type() const { return _conversation_type; }
    void conversation_type(ConversationType v) { _conversation_type = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    int member_count() const { return _member_count; }
    void member_count(int v) { _member_count = v; }

    ConversationStatus status() const { return _status; }
    void status(ConversationStatus v) { _status = v; }

    std::string avatar_id() const { return _avatar_id ? *_avatar_id : std::string(); }
    void avatar_id(const std::string &v) { _avatar_id = v; }

    std::string owner_id() const { return _owner_id ? *_owner_id : std::string(); }
    void owner_id(const std::string &v) { _owner_id = v; }

    std::string peer_user_id() const { return _peer_user_id ? *_peer_user_id : std::string(); }
    void peer_user_id(const std::string &v) { _peer_user_id = v; }

    std::string description() const { return _description ? *_description : std::string(); }
    void description(const std::string &v) { _description = v; }

    std::string announcement() const { return _announcement ? *_announcement : std::string(); }
    void announcement(const std::string &v) { _announcement = v; }

    bool muted_all() const { return _muted_all; }
    void muted_all(bool v) { _muted_all = v; }

    unsigned long max_seq() const { return _max_seq; }
    void max_seq(unsigned long v) { _max_seq = v; }

    boost::posix_time::ptime update_time() const { return _update_time; }
    void update_time(const boost::posix_time::ptime &v) { _update_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)") index unique
    std::string _conversation_id;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _conversation_name;

    #pragma db type("tinyint unsigned")
    ConversationType _conversation_type {ConversationType::PRIVATE};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db type("int unsigned")
    int _member_count {0};

    #pragma db type("tinyint unsigned")
    ConversationStatus _status {ConversationStatus::NORMAL};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _avatar_id;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _owner_id;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _peer_user_id;

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _description;

    #pragma db type("varchar(1024)")
    odb::nullable<std::string> _announcement;

    #pragma db type("tinyint(1)")
    bool _muted_all {false};

    #pragma db type("bigint unsigned")
    unsigned long _max_seq {0};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;
};

} // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add odb/conversation.hxx
git commit -m "odb: 新增 Conversation 实体（替代 chat_session）"
```

---

## Task 3: 新建 ODB ConversationMember 实体（替代 chat_session_member.hxx）

**Files:**
- Create: `odb/conversation_member.hxx`

- [ ] **Step 1: 写 odb/conversation_member.hxx**

```cpp
#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * 会话成员表 (conversation_member) — 替代旧 chat_session_member.hxx
 * 字段集合不变，仅类名/枚举名/表名 rename：
 *   ChatSessionMember  → ConversationMember
 *   ChatSessionRole    → MemberRole          (NORMAL/ADMIN/OWNER)
 *   table chat_session_member → table conversation_member
 *   _session_id        → _conversation_id
 */

namespace chatnow
{

enum class MemberRole : unsigned char {
    NORMAL = 0, ADMIN = 1, OWNER = 2
};

enum class JoinSource : unsigned char {
    UNKNOWN   = 0,
    INVITE    = 1,
    SEARCH    = 2,
    QR_CODE   = 3,
    LINK      = 4,
    ADMIN_ADD = 5,
    CREATE    = 6
};

#pragma db object table("conversation_member")
class ConversationMember
{
public:
    ConversationMember() = default;
    ConversationMember(const std::string &cid,
                       const std::string &uid,
                       bool muted,
                       bool visible,
                       MemberRole role,
                       const boost::posix_time::ptime &join_time)
        : _conversation_id(cid), _user_id(uid),
          _muted(muted), _visible(visible),
          _role(role), _join_time(join_time) {}

    std::string conversation_id() const { return _conversation_id; }
    void conversation_id(const std::string &v) { _conversation_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    unsigned long last_read_seq() const { return _last_read_seq; }
    void last_read_seq(unsigned long v) { _last_read_seq = v; }

    unsigned long last_ack_seq() const { return _last_ack_seq; }
    void last_ack_seq(unsigned long v) { _last_ack_seq = v; }

    bool muted() const { return _muted; }
    void muted(bool v) { _muted = v; }

    bool visible() const { return _visible; }
    void visible(bool v) { _visible = v; }

    boost::posix_time::ptime pin_time() const {
        return _pin_time ? *_pin_time : boost::posix_time::ptime();
    }
    void pin_time(const boost::posix_time::ptime &v) { _pin_time = v; }
    void unpin() { _pin_time = odb::nullable<boost::posix_time::ptime>(); }
    bool is_pinned() const { return static_cast<bool>(_pin_time); }

    MemberRole role() const { return _role; }
    void role(MemberRole v) { _role = v; }

    boost::posix_time::ptime join_time() const { return _join_time; }
    void join_time(const boost::posix_time::ptime &v) { _join_time = v; }

    boost::posix_time::ptime mute_until() const {
        return _mute_until ? *_mute_until : boost::posix_time::ptime();
    }
    void mute_until(const boost::posix_time::ptime &v) { _mute_until = v; }

    std::string alias() const { return _alias ? *_alias : std::string(); }
    void alias(const std::string &v) { _alias = v; }

    std::string inviter_id() const { return _inviter_id ? *_inviter_id : std::string(); }
    void inviter_id(const std::string &v) { _inviter_id = v; }

    JoinSource join_source() const { return _join_source; }
    void join_source(JoinSource v) { _join_source = v; }

    bool is_quit() const { return _is_quit; }
    void is_quit(bool v) { _is_quit = v; }

    boost::posix_time::ptime quit_time() const {
        return _quit_time ? *_quit_time : boost::posix_time::ptime();
    }
    void quit_time(const boost::posix_time::ptime &v) { _quit_time = v; }

    std::string draft() const { return _draft ? *_draft : std::string(); }
    void draft(const std::string &v) { _draft = v; }
    void clear_draft() { _draft = odb::nullable<std::string>(); }
    bool has_draft() const { return static_cast<bool>(_draft); }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _conversation_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("bigint unsigned")
    unsigned long _last_read_seq {0};

    #pragma db type("bigint unsigned")
    unsigned long _last_ack_seq {0};

    #pragma db type("tinyint(1)")
    bool _muted {false};

    #pragma db type("tinyint(1)")
    bool _visible {true};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _pin_time;

    #pragma db type("tinyint unsigned")
    MemberRole _role {MemberRole::NORMAL};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _alias;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _inviter_id;

    #pragma db type("tinyint unsigned")
    JoinSource _join_source {JoinSource::UNKNOWN};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _join_time;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _mute_until;

    #pragma db type("tinyint(1)")
    bool _is_quit {false};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _quit_time;

    #pragma db type("text")
    odb::nullable<std::string> _draft;

    #pragma db index("uk_conv_user") unique members(_conversation_id, _user_id)
    #pragma db index("idx_user_conv") members(_user_id, _is_quit, _conversation_id)
};

} // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add odb/conversation_member.hxx
git commit -m "odb: 新增 ConversationMember 实体（替代 chat_session_member）"
```

---

## Task 4: 新建 ODB OrderedConversationView（替代 chat_session_view.hxx）

**Files:**
- Create: `odb/conversation_view.hxx`

- [ ] **Step 1: 写 odb/conversation_view.hxx**

```cpp
#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "conversation.hxx"
#include "conversation_member.hxx"

/**
 * 会话列表视图 OrderedConversationView — 替代 OrderedChatSessionView
 *  字段集合不变；类名/列别名 rename。
 */

namespace chatnow
{

#pragma db view                                                              \
    object(Conversation = c)                                                 \
    object(ConversationMember = m : c::_conversation_id == m::_conversation_id)
struct OrderedConversationView
{
    #pragma db column(c::_conversation_id)
    std::string conversation_id;

    #pragma db column(c::_conversation_name)
    odb::nullable<std::string> conversation_name;

    #pragma db column(c::_conversation_type)
    ConversationType conversation_type;

    #pragma db column(c::_create_time)
    boost::posix_time::ptime create_time;

    #pragma db column(c::_member_count)
    int member_count;

    #pragma db column(c::_status)
    ConversationStatus status;

    #pragma db column(c::_avatar_id)
    odb::nullable<std::string> avatar_id;

    #pragma db column(c::_owner_id)
    odb::nullable<std::string> owner_id;

    #pragma db column(c::_peer_user_id)
    odb::nullable<std::string> peer_user_id;

    #pragma db column(c::_max_seq)
    unsigned long max_seq;

    #pragma db column(c::_muted_all)
    bool muted_all;

    #pragma db column(c::_update_time)
    boost::posix_time::ptime update_time;

    #pragma db column(m::_user_id)
    std::string user_id;

    #pragma db column(m::_pin_time)
    odb::nullable<boost::posix_time::ptime> pin_time;

    #pragma db column(m::_muted)
    bool muted;

    #pragma db column(m::_visible)
    bool visible;

    #pragma db column(m::_role)
    MemberRole role;

    #pragma db column(m::_alias)
    odb::nullable<std::string> alias;

    #pragma db column(m::_last_read_seq)
    unsigned long last_read_seq;

    #pragma db column(m::_last_ack_seq)
    unsigned long last_ack_seq;

    #pragma db column(m::_mute_until)
    odb::nullable<boost::posix_time::ptime> mute_until;

    #pragma db column(m::_join_time)
    boost::posix_time::ptime join_time;

    #pragma db column(m::_is_quit)
    bool is_quit;

    #pragma db column(m::_draft)
    odb::nullable<std::string> draft;
};

} // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add odb/conversation_view.hxx
git commit -m "odb: 新增 OrderedConversationView（替代 OrderedChatSessionView）"
```

---

## Task 5: 新建 ConversationTable DAO

**Files:**
- Create: `common/dao/mysql_conversation.hpp`

- [ ] **Step 1: 写 common/dao/mysql_conversation.hpp**

完全照搬 `common/dao/mysql_chat_session.hpp` 接口形状，替换类型 / 字段名 / 表名 / 类名：

```cpp
#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "conversation.hxx"
#include "conversation-odb.hxx"
#include "conversation_view.hxx"
#include "conversation_view-odb.hxx"
#include "conversation_member.hxx"
#include "conversation_member-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <memory>

namespace chatnow
{

class ConversationTable
{
public:
    using ptr = std::shared_ptr<ConversationTable>;
    ConversationTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增会话；自动写 create_time / update_time */
    bool insert(Conversation &c) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            c.create_time(now);
            c.update_time(now);

            odb::transaction trans(_db->begin());
            _db->persist(c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增会话失败 {}: {}", c.conversation_name(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 软删除：status=DISMISSED + 刷 update_time */
    bool update_status(const std::string &cid, ConversationStatus s) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> c(_db->query_one<Conversation>(
                query::conversation_id == cid));
            if(!c) {
                trans.commit();
                return false;
            }
            c->status(s);
            c->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话 status 失败 {}: {}", cid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 通过会话 ID 查会话 */
    std::shared_ptr<Conversation> select(const std::string &cid) {
        std::shared_ptr<Conversation> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            res.reset(_db->query_one<Conversation>(query::conversation_id == cid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询会话失败 {}: {}", cid, e.what());
        }
        return res;
    }

    /* brief: 通过对端 user_id 取单聊会话 */
    std::shared_ptr<Conversation> select_private_by_peer(const std::string &uid, const std::string &pid) {
        std::shared_ptr<Conversation> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            res.reset(_db->query_one<Conversation>(
                query::conversation_type == ConversationType::PRIVATE &&
                query::peer_user_id == pid));
            (void)uid;
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过对端查单聊失败 {}-{}: {}", uid, pid, e.what());
        }
        return res;
    }

    bool private_exists(const std::string &uid, const std::string &pid) {
        return static_cast<bool>(select_private_by_peer(uid, pid));
    }

    bool exists(const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> r(_db->query_one<Conversation>(
                query::conversation_id == cid));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("判断会话是否存在失败 {}: {}", cid, e.what());
            return false;
        }
    }

    bool update(const std::shared_ptr<Conversation> &c) {
        try {
            c->update_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->update(*c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话失败 {}: {}", c->conversation_id(), e.what());
            return false;
        }
        return true;
    }

    bool bump_max_seq(const std::string &cid, unsigned long new_seq) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Conversation>;
            std::shared_ptr<Conversation> c(_db->query_one<Conversation>(
                (query::conversation_id == cid) + " FOR UPDATE"));
            if(!c) {
                trans.commit();
                return false;
            }
            if(c->max_seq() < new_seq) {
                c->max_seq(new_seq);
                _db->update(*c);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("刷新 max_seq 失败 {}: {}", cid, e.what());
            return false;
        }
        return true;
    }

    std::vector<Conversation> select(const std::vector<std::string> &cids) {
        std::vector<Conversation> res;
        if(cids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Conversation>;
            using result = odb::result<Conversation>;
            result r(_db->query<Conversation>(
                query::conversation_id.in_range(cids.begin(), cids.end())));
            for(auto &c : r) res.push_back(c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量取会话失败: {}", e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
```

- [ ] **Step 2: Commit**

```bash
git add common/dao/mysql_conversation.hpp
git commit -m "dao: 新增 ConversationTable（替代 ChatSessionTable）"
```

---

## Task 6: 新建 ConversationMemberTable DAO（含 4 个新方法）

**Files:**
- Create: `common/dao/mysql_conversation_member.hpp`
- Reference: `common/dao/mysql_chat_session_member.hpp`（照搬接口形状，rename + 加 4 方法）

- [ ] **Step 1: 复制 mysql_chat_session_member.hpp 全文到 mysql_conversation_member.hpp**

操作：
```bash
cp common/dao/mysql_chat_session_member.hpp common/dao/mysql_conversation_member.hpp
```

- [ ] **Step 2: 在新文件里逐项 rename**

打开 `common/dao/mysql_conversation_member.hpp`，整文件 replace（保留接口形状，只改类型/字段/表名）：

| 旧 | 新 |
|---|---|
| `ChatSessionMember` | `ConversationMember` |
| `ChatSessionMemberTable` | `ConversationMemberTable` |
| `ChatSessionRole` | `MemberRole` |
| `chat_session_member.hxx` | `conversation_member.hxx` |
| `chat_session_member-odb.hxx` | `conversation_member-odb.hxx` |
| `chat_session.hxx` / `chat_session-odb.hxx` 引用 | `conversation.hxx` / `conversation-odb.hxx` |
| `chat_session_view.hxx` / 同 odb | `conversation_view.hxx` / 同 odb |
| `class ChatSession` 引用（如有 query<ChatSession>） | `class Conversation` |
| `query::session_id` / `member::_session_id` | `query::conversation_id` / `member::_conversation_id` |
| `OrderedChatSessionView` | `OrderedConversationView` |
| `cs::_chat_session_id` / `cm::_session_id` 等列别名 | `c::_conversation_id` / `m::_conversation_id` |
| `static_cast<ChatSessionRole>` | `static_cast<MemberRole>` |

> 工具提示：`sed -i 's/ChatSessionMember/ConversationMember/g; s/ChatSessionRole/MemberRole/g; s/ChatSessionTable/ConversationTable/g; s/chat_session_member/conversation_member/g; s/chat_session/conversation/g; s/_session_id/_conversation_id/g; s/OrderedChatSessionView/OrderedConversationView/g' common/dao/mysql_conversation_member.hpp` 后再人工通读 + 修头注释。注意 sed 替换可能误改 ES 字符串字面量，本文件无该字面量（只 DAO 层），但完成后 grep 一遍确认。

- [ ] **Step 3: 在新文件末尾追加 4 个新方法**

在 `class ConversationMemberTable` 内（`private:` 之前）追加：

```cpp
    /* brief: SaveDraft 写入。draft 空字符串视作清除 */
    bool update_draft(const std::string &cid, const std::string &uid,
                      const std::string &draft) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ConversationMember>;
            std::shared_ptr<ConversationMember> m(_db->query_one<ConversationMember>(
                query::conversation_id == cid && query::user_id == uid));
            if(!m) { trans.commit(); return false; }
            if(draft.empty()) m->clear_draft();
            else              m->draft(draft);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("保存草稿失败 {}-{}: {}", cid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: MarkRead 写入；只在 new_seq > old 时更新（防回退） */
    bool update_last_read_seq(const std::string &cid, const std::string &uid,
                              unsigned long new_seq) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ConversationMember>;
            std::shared_ptr<ConversationMember> m(_db->query_one<ConversationMember>(
                query::conversation_id == cid && query::user_id == uid));
            if(!m) { trans.commit(); return false; }
            if(m->last_read_seq() < new_seq) {
                m->last_read_seq(new_seq);
                _db->update(*m);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新已读游标失败 {}-{}: {}", cid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 设置成员角色 */
    bool update_member_role(const std::string &cid, const std::string &uid,
                            MemberRole role) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ConversationMember>;
            std::shared_ptr<ConversationMember> m(_db->query_one<ConversationMember>(
                query::conversation_id == cid && query::user_id == uid));
            if(!m) { trans.commit(); return false; }
            m->role(role);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新成员角色失败 {}-{}: {}", cid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 取自身的 SelfMemberInfo 所需所有字段（一行） */
    std::shared_ptr<ConversationMember> select_self(const std::string &cid,
                                                    const std::string &uid) {
        std::shared_ptr<ConversationMember> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ConversationMember>;
            res.reset(_db->query_one<ConversationMember>(
                query::conversation_id == cid && query::user_id == uid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询成员失败 {}-{}: {}", cid, uid, e.what());
        }
        return res;
    }
```

> 注：原 `mysql_chat_session_member.hpp` 已经有 `update_role(...)` 方法。本文件**保留原方法**（已 rename 成员变量），上面追加的 `update_member_role` 是显式新别名供 §11 ChangeMemberRole/TransferOwner 调用使用；命名冲突时以 `update_member_role` 为准并删旧 `update_role`。Step 4 校验。

- [ ] **Step 4: 校验编译信号面**

Run: `grep -n "update_role\|update_member_role" common/dao/mysql_conversation_member.hpp`
Expected: 至少 1 行命中；如果 `update_role` 已存在，则**保留 `update_role`，删除 Step 3 新加的 `update_member_role`**（避免重复方法），后续 handler 调 `update_role`。

- [ ] **Step 5: Commit**

```bash
git add common/dao/mysql_conversation_member.hpp
git commit -m "dao: 新增 ConversationMemberTable + 3 个新方法（draft/last_read_seq/select_self）"
```

---

## Task 7: ESChatSession 类改名为 ESConversation

**Files:**
- Modify: `common/dao/data_es.hpp`

- [ ] **Step 1: 把 class ESChatSession 改名为 class ESConversation**

打开 `common/dao/data_es.hpp`，找到 `class ESChatSession` 段（约 218–290 行）。**只改类名 + 函数 `append_data` 入参类型 + 函数 `search` 入参枚举类型**，**ES index 字符串字面量 "chat_session" 一律保留**（YAGNI 不重建索引）。

```cpp
class ESConversation
{
public:
    using ptr = std::shared_ptr<ESConversation>;
    explicit ESConversation(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}

    bool create_index() {
        bool ret = ESIndex(_client, "chat_session")
            .append("chat_session_id",   "keyword", "standard", true)
            .append("chat_session_name")
            .append("chat_session_type", "integer", "standard", false)
            .append("avatar_id",         "keyword", "standard", false)
            .append("status",            "integer", "standard", false)
            .append("update_time",       "long",    "standard", false)
            .create();
        if(!ret) {
            LOG_ERROR("会话搜索索引创建失败");
            return false;
        }
        LOG_INFO("会话搜索索引创建成功");
        return true;
    }

    bool append_data(const chatnow::Conversation &c) {
        static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        long ts = (c.update_time() - epoch).total_seconds();
        bool ret = ESInsert(_client, "chat_session")
            .append("chat_session_id",   c.conversation_id())
            .append("chat_session_name", c.conversation_name())
            .append("chat_session_type", static_cast<int>(c.conversation_type()))
            .append("avatar_id",         c.avatar_id())
            .append("status",            static_cast<int>(c.status()))
            .append("update_time",       ts)
            .insert(c.conversation_id());
        if(!ret) {
            LOG_ERROR("会话搜索数据插入/更新失败 cid={}", c.conversation_id());
            return false;
        }
        return true;
    }

    bool remove(const std::string &cid) {
        return ESRemove(_client, "chat_session").remove(cid);
    }

    std::vector<std::string> search(const std::string &key,
                                    std::optional<chatnow::ConversationType> type = std::nullopt,
                                    int size = 20)
    {
        std::vector<std::string> res;
        ESSearch builder(_client, "chat_session");
        builder.append_must_match("chat_session_name", key)
               .append_must_term("status", std::to_string(0))
               .sort_by("update_time", "desc")
               .page(0, size);
        if(type.has_value()) {
            builder.append_must_term("chat_session_type",
                                     std::to_string(static_cast<int>(type.value())));
        }
        Json::Value json_session = builder.search();
        if(!json_session.isArray()) return res;
        for(int i = 0; i < (int)json_session.size(); ++i) {
            res.push_back(json_session[i]["_source"]["chat_session_id"].asString());
        }
        return res;
    }

private:
    std::shared_ptr<elasticlient::Client> _client;
};
```

- [ ] **Step 2: Commit**

```bash
git add common/dao/data_es.hpp
git commit -m "dao(es): ESChatSession 改名 ESConversation（索引名字面量保留）"
```

---

## Task 8: Members Redis key 前缀 rename（语义对齐）

**Files:**
- Modify: `common/dao/data_redis.hpp`

- [ ] **Step 1: 改 kMembers 前缀**

打开 `common/dao/data_redis.hpp`，找到约 41 行：

```cpp
inline constexpr const char* kMembers    = "im:members:";       // ssid       -> SET<user_id>
```

改为：

```cpp
inline constexpr const char* kMembers    = "im:conversation:members:"; // cid -> SET<user_id>
```

- [ ] **Step 2: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "dao(redis): Members key 前缀 im:members → im:conversation:members"
```

---

## Task 9: 新建 conversation 服务骨架（h / cc / CMakeLists / Dockerfile / conf）

**Files:**
- Create: `conversation/CMakeLists.txt`
- Create: `conversation/Dockerfile`
- Create: `conversation/source/conversation_server.h`（仅骨架，handler 在后续 task 填充）
- Create: `conversation/source/conversation_server.cc`
- Create: `conf/conversation_server.conf`

- [ ] **Step 1: 写 conversation/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.1.3)
project(conversation_server)

set(target "conversation_server")

# 1. proto
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files common/types.proto common/error.proto common/envelope.proto
                message/message_types.proto
                message/message_service.proto
                identity/identity_service.proto
                conversation/conversation_service.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path}
                 --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

# 2. ODB
set(odb_path ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
set(odb_files conversation_member.hxx conversation.hxx conversation_view.hxx)
set(odb_srcs "")
foreach(odb_file ${odb_files})
    string(REPLACE ".hxx" "-odb.cxx" odb_cxx ${odb_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${odb_cxx})
        add_custom_command(
            PRE_BUILD
            COMMAND odb
            ARGS -d mysql --std c++11 --generate-query --generate-schema
                 --profile boost/date-time ${odb_path}/${odb_file}
            DEPENDS ${odb_path}/${odb_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
            COMMENT "生成ODB框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
        )
    endif()
    list(APPEND odb_srcs ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx})
endforeach()

# 3. 源码
set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)

add_executable(${target} ${src_files} ${proto_srcs} ${odb_srcs})

target_link_libraries(${target} -lgflags -lspdlog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcpprest -lcurl -lodb-mysql -lodb -lodb-boost
    -lcpr -lelasticlient -ljsoncpp
    -lhiredis -lredis++)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../odb)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
```

- [ ] **Step 2: 写 conversation/Dockerfile**

```dockerfile
# 声明基础镜像来源
FROM ubuntu:24.04
WORKDIR /im
RUN mkdir -p /im/logs && mkdir -p /im/data && mkdir -p /im/conf && mkdir -p /im/bin
COPY ./build/conversation_server /im/bin
COPY ./depends/* /lib/x86_64-linux-gnu/
COPY ./nc /bin
CMD /im/bin/conversation_server -flagfile=/im/conf/conversation_server.conf
```

- [ ] **Step 3: 写 conf/conversation_server.conf**

```
-run_mode=true
-log_file=/im/logs/conversation.log
-log_level=0
-registry_host=http://10.0.4.10:2379
-base_service=/service
-instance_name=/conversation_service/instance
-access_host=10.0.4.10:10007
-listen_port=10007
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-media_service=/service/media_service
-message_service=/service/message_service
-es_host=http://10.0.4.10:9200/
-redis_host=10.0.4.10
-redis_port=6379
-redis_db=0
-redis_keep_alive=true
-redis_pool_size=4
-mysql_host=10.0.4.10
-mysql_user=root
-mysql_pswd=YHY060403
-mysql_db=chatnow
-mysql_cset=utf8mb4
-mysql_port=0
-mysql_pool_count=4
-public_url_prefix=http://10.0.4.10:9000/chatnow-media-public
```

- [ ] **Step 4: 写 conversation/source/conversation_server.cc（main 入口）**

```cpp
#include "conversation_server.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(registry_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(instance_name, "/conversation_service/instance", "服务实例 etcd 路径");
DEFINE_string(access_host, "127.0.0.1:10007", "当前实例的外部访问地址");

DEFINE_int32(listen_port, 10007, "RPC服务器监听端口");
DEFINE_int32(rpc_timeout, -1, "RPC调用超时时间");
DEFINE_int32(rpc_threads, 1, "RPC的IO线程数量");

DEFINE_string(identity_service, "/service/identity_service", "Identity 子服务名（GetMultiUserInfo）");
DEFINE_string(media_service,    "/service/media_service",    "Media 子服务名（avatar 路径生成保留）");
DEFINE_string(message_service,  "/service/message_service",  "Message 子服务名（SyncMessages 取 last_message）");

DEFINE_string(es_host, "http://127.0.0.1:9200/", "ES搜索引擎服务器URL");

DEFINE_string(redis_host, "127.0.0.1", "Redis 服务器访问地址");
DEFINE_int32(redis_port, 6379, "Redis 服务器访问端口");
DEFINE_int32(redis_db, 0, "Redis 选择的库");
DEFINE_bool(redis_keep_alive, true, "Redis 长连接");
DEFINE_int32(redis_pool_size, 4, "Redis 连接池大小");

DEFINE_string(mysql_host, "127.0.0.1", "MySQL服务器访问地址");
DEFINE_string(mysql_user, "root", "MySQL访问服务器用户名");
DEFINE_string(mysql_pswd, "", "MySQL服务器访问密码");
DEFINE_string(mysql_db, "chatnow", "MySQL默认库名称");
DEFINE_string(mysql_cset, "utf8mb4", "MySQL客户端字符集");
DEFINE_int32(mysql_port, 0, "MySQL服务器访问端口");
DEFINE_int32(mysql_pool_count, 4, "MySQL连接池最大连接数量");

DEFINE_string(public_url_prefix, "http://127.0.0.1:9000/chatnow-media-public",
              "Media 公开 bucket URL 前缀（avatar_file_id → URL 转换用）");

int main(int argc, char *argv[])
{
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    chatnow::ConversationServerBuilder csb;
    csb.make_es_object({FLAGS_es_host});
    csb.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                          FLAGS_redis_keep_alive, FLAGS_redis_pool_size);
    csb.make_mysql_object(FLAGS_mysql_user, FLAGS_mysql_pswd, FLAGS_mysql_host,
                          FLAGS_mysql_db, FLAGS_mysql_cset,
                          FLAGS_mysql_port, FLAGS_mysql_pool_count);
    csb.make_discovery_object(FLAGS_registry_host, FLAGS_base_service,
                              FLAGS_identity_service, FLAGS_media_service,
                              FLAGS_message_service);
    csb.make_config(FLAGS_public_url_prefix);
    csb.make_rpc_object(FLAGS_listen_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    csb.make_registry_object(FLAGS_registry_host,
                             FLAGS_base_service + FLAGS_instance_name,
                             FLAGS_access_host);

    auto server = csb.build();
    server->start();
    return 0;
}
```

- [ ] **Step 5: 写 conversation/source/conversation_server.h（骨架；handler 留空，T10–T14 填充）**

```cpp
#pragma once

#include <brpc/server.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "utils/utils.hpp"
#include "dao/data_es.hpp"
#include "dao/data_redis.hpp"
#include "dao/mysql_conversation.hpp"
#include "dao/mysql_conversation_member.hpp"
#include "common/types.pb.h"
#include "common/error.pb.h"
#include "common/envelope.pb.h"
#include "identity/identity_service.pb.h"
#include "message/message_service.pb.h"
#include "message/message_types.pb.h"
#include "conversation/conversation_service.pb.h"

#include "auth/auth_context.hpp"
#include "auth/forward_auth.hpp"
#include "error/error_codes.hpp"
#include "error/handle_rpc.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"

namespace chatnow {

struct ConversationServiceConfig {
    std::string public_url_prefix;
};

class ConversationServiceImpl : public ::chatnow::conversation::ConversationService
{
public:
    ConversationServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                            const std::shared_ptr<odb::core::database> &mysql_client,
                            const Members::ptr &members_cache,
                            const ServiceManager::ptr &channel_manager,
                            const std::string &identity_service_name,
                            const std::string &media_service_name,
                            const std::string &message_service_name,
                            const ConversationServiceConfig &cfg)
        : _es_conv(std::make_shared<ESConversation>(es_client)),
          _mysql_conv(std::make_shared<ConversationTable>(mysql_client)),
          _mysql_member(std::make_shared<ConversationMemberTable>(mysql_client)),
          _members_cache(members_cache),
          _identity_service_name(identity_service_name),
          _media_service_name(media_service_name),
          _message_service_name(message_service_name),
          _mm_channels(channel_manager),
          _cfg(cfg) {}

    ~ConversationServiceImpl() override = default;

    // —— 18 个 RPC 在 T10–T14 中填充实现 ——
    // 占位：先以 NOT_IMPLEMENTED 直接返回，便于本 task 的服务骨架可编译
    #define _PLACEHOLDER_RPC(Method, Req, Rsp) \
        void Method(::google::protobuf::RpcController* base_cntl, \
                    const ::chatnow::conversation::Req* req, \
                    ::chatnow::conversation::Rsp* rsp, \
                    ::google::protobuf::Closure* done) override { \
            brpc::ClosureGuard done_guard(done); \
            (void)base_cntl; (void)req; \
            rsp->mutable_header()->set_success(false); \
            rsp->mutable_header()->set_error_code( \
                ::chatnow::error::kSystemInternalError); \
            rsp->mutable_header()->set_error_message("not implemented"); \
        }
    _PLACEHOLDER_RPC(ListConversations, ListConversationsReq, ListConversationsRsp)
    _PLACEHOLDER_RPC(GetConversation,   GetConversationReq,   GetConversationRsp)
    _PLACEHOLDER_RPC(CreateConversation,CreateConversationReq,CreateConversationRsp)
    _PLACEHOLDER_RPC(UpdateConversation,UpdateConversationReq,UpdateConversationRsp)
    _PLACEHOLDER_RPC(DismissConversation,DismissConversationReq,DismissConversationRsp)
    _PLACEHOLDER_RPC(AddMembers,        AddMembersReq,        AddMembersRsp)
    _PLACEHOLDER_RPC(RemoveMembers,     RemoveMembersReq,     RemoveMembersRsp)
    _PLACEHOLDER_RPC(TransferOwner,     TransferOwnerReq,     TransferOwnerRsp)
    _PLACEHOLDER_RPC(ChangeMemberRole,  ChangeMemberRoleReq,  ChangeMemberRoleRsp)
    _PLACEHOLDER_RPC(ListMembers,       ListMembersReq,       ListMembersRsp)
    _PLACEHOLDER_RPC(SetMute,           SetMuteReq,           SetMuteRsp)
    _PLACEHOLDER_RPC(SetPin,            SetPinReq,            SetPinRsp)
    _PLACEHOLDER_RPC(SetVisible,        SetVisibleReq,        SetVisibleRsp)
    _PLACEHOLDER_RPC(QuitConversation,  QuitConversationReq,  QuitConversationRsp)
    _PLACEHOLDER_RPC(MarkRead,          MarkReadReq,          MarkReadRsp)
    _PLACEHOLDER_RPC(SaveDraft,         SaveDraftReq,         SaveDraftRsp)
    _PLACEHOLDER_RPC(SearchConversations,SearchConversationsReq,SearchConversationsRsp)
    _PLACEHOLDER_RPC(GetMemberIds,      GetMemberIdsReq,      GetMemberIdsRsp)
    #undef _PLACEHOLDER_RPC

private:
    ESConversation::ptr           _es_conv;
    ConversationTable::ptr        _mysql_conv;
    ConversationMemberTable::ptr  _mysql_member;
    Members::ptr                  _members_cache;
    std::string                   _identity_service_name;
    std::string                   _media_service_name;
    std::string                   _message_service_name;
    ServiceManager::ptr           _mm_channels;
    ConversationServiceConfig     _cfg;
};

class ConversationServer
{
public:
    using ptr = std::shared_ptr<ConversationServer>;
    ConversationServer(const Registry::ptr &reg_client,
                       const std::shared_ptr<brpc::Server> &server)
        : _reg_client(reg_client), _rpc_server(server) {}

    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    Registry::ptr               _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
};

class ConversationServerBuilder
{
public:
    void make_es_object(const std::vector<std::string> &host_list) {
        _es_client = std::make_shared<elasticlient::Client>(host_list);
        ESConversation(_es_client).create_index();
    }

    void make_redis_object(const std::string &host, int port, int db,
                           bool keep_alive, int pool_size) {
        _redis_client = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _members_cache = std::make_shared<Members>(_redis_client);
    }

    void make_mysql_object(const std::string &user, const std::string &pswd,
                           const std::string &host, const std::string &dbname,
                           const std::string &cset, int port, int pool_count) {
        _mysql_client = ODBFactory::create(user, pswd, host, dbname, cset, port, pool_count);
    }

    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_service,
                               const std::string &identity_service_name,
                               const std::string &media_service_name,
                               const std::string &message_service_name) {
        _identity_service_name    = identity_service_name;
        _media_service_name       = media_service_name;
        _message_service_name     = message_service_name;
        _mm_channels              = std::make_shared<ServiceManager>();
        _mm_channels->declared(identity_service_name);
        _mm_channels->declared(media_service_name);
        _mm_channels->declared(message_service_name);

        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _dis_client = std::make_shared<Discovery>(reg_host, base_service, put_cb, del_cb);
    }

    void make_config(const std::string &public_url_prefix) {
        _cfg.public_url_prefix = public_url_prefix;
    }

    void make_rpc_object(int port, int timeout, int num_threads) {
        if(!_es_client || !_mysql_client || !_mm_channels) {
            LOG_ERROR("ConversationServerBuilder: 请先初始化所有依赖");
            abort();
        }
        _rpc_server = std::make_shared<brpc::Server>();
        ConversationServiceImpl *svc = new ConversationServiceImpl(
            _es_client, _mysql_client, _members_cache, _mm_channels,
            _identity_service_name, _media_service_name, _message_service_name, _cfg);
        if(_rpc_server->AddService(svc, brpc::SERVER_OWNS_SERVICE) != 0) {
            LOG_ERROR("AddService 失败");
            abort();
        }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads      = num_threads;
        if(_rpc_server->Start(port, &options) != 0) {
            LOG_ERROR("brpc::Server::Start 失败");
            abort();
        }
    }

    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }

    ConversationServer::ptr build() {
        return std::make_shared<ConversationServer>(_reg_client, _rpc_server);
    }

private:
    std::shared_ptr<elasticlient::Client>   _es_client;
    std::shared_ptr<sw::redis::Redis>       _redis_client;
    Members::ptr                            _members_cache;
    std::shared_ptr<odb::core::database>    _mysql_client;
    Discovery::ptr                          _dis_client;
    Registry::ptr                           _reg_client;
    ServiceManager::ptr                     _mm_channels;
    std::string                             _identity_service_name;
    std::string                             _media_service_name;
    std::string                             _message_service_name;
    ConversationServiceConfig               _cfg;
    std::shared_ptr<brpc::Server>           _rpc_server;
};

} // namespace chatnow
```

> 此 task 之后服务可"编译过 + 启动"（前提 message_service 阻塞解决；本期不要求验证），所有 RPC 返回 `not implemented`，T10–T14 替换占位实现。

- [ ] **Step 6: 顶层 CMakeLists 加 add_subdirectory**

打开 `CMakeLists.txt`（根），把 `add_subdirectory(chatsession)` 改为 `add_subdirectory(conversation)`（保留 chatsession 旧目录暂不删，最后 T17 一起清）。

如果根 CMakeLists 没有 chatsession 行，直接追加：

```cmake
add_subdirectory(conversation)
```

- [ ] **Step 7: Commit**

```bash
git add conversation/ conf/conversation_server.conf CMakeLists.txt
git commit -m "$(cat <<'EOF'
conversation: 新建服务骨架（CMakeLists / Builder / main / Dockerfile）

服务骨架仿 Identity / Relationship 模式。RPC 全部占位 NOT_IMPLEMENTED，
T10–T14 逐组替换为真实实现。
EOF
)"
```

---

## Task 10: 实现 5 个读取类 RPC

**Files:**
- Modify: `conversation/source/conversation_server.h`

> 每个 RPC 内删除 §9 的 `_PLACEHOLDER_RPC(...)` 行，替换为下面的真实实现。下面所有 handler 都使用 `HANDLE_RPC` 宏，`auth.user_id` 直接可用。

- [ ] **Step 1: 删除 ListConversations 占位 + 加入私有辅助 + 实现 ListConversations**

在 `class ConversationServiceImpl` 的 `private:` 段（成员变量之前）追加内部辅助：

```cpp
private:
    // —— 内部辅助 ——
    bool fetch_user_infos_(brpc::Controller* in_cntl, const std::string& rid,
                           const std::vector<std::string>& uids,
                           std::unordered_map<std::string, ::chatnow::common::UserInfo>& out)
    {
        if (uids.empty()) return true;
        auto channel = _mm_channels->choose(_identity_service_name);
        if(!channel) {
            LOG_ERROR("rid={} identity service unavailable (channel null)", rid);
            return false;
        }
        ::chatnow::identity::IdentityService_Stub stub(channel.get());
        ::chatnow::identity::GetMultiUserInfoReq  ireq;
        ::chatnow::identity::GetMultiUserInfoRsp  irsp;
        ireq.set_request_id(rid);
        for (auto &u : uids) ireq.add_users_id(u);
        brpc::Controller out_cntl;
        chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.GetMultiUserInfo(&out_cntl, &ireq, &irsp, nullptr);
        if(out_cntl.Failed() || !irsp.header().success()) {
            LOG_ERROR("rid={} GetMultiUserInfo failed: {}", rid, out_cntl.ErrorText());
            return false;
        }
        for (auto &kv : irsp.users_info()) out.insert({kv.first, kv.second});
        return true;
    }

    // 调 MessageService.SyncMessages(after_seq, limit=1) 取 last_message。
    // 失败/不可达返回 nullopt（fail-soft）。Message 服务未迁前**编译期**会因
    // SyncMessages stub 缺生成代码失败 — 这是预期阻塞（spec §7 验收 #2）。
    bool fetch_last_message_(brpc::Controller* in_cntl, const std::string& rid,
                             const std::string& cid, unsigned long after_seq,
                             ::chatnow::message::MessagePreview& out)
    {
        auto channel = _mm_channels->choose(_message_service_name);
        if(!channel) return false;
        ::chatnow::message::MessageService_Stub stub(channel.get());
        ::chatnow::message::SyncMessagesReq  mreq;
        ::chatnow::message::SyncMessagesRsp  mrsp;
        mreq.set_request_id(rid);
        mreq.set_conversation_id(cid);
        mreq.set_after_seq(after_seq);
        mreq.set_limit(1);
        brpc::Controller out_cntl;
        chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl);
        stub.SyncMessages(&out_cntl, &mreq, &mrsp, nullptr);
        if(out_cntl.Failed() || !mrsp.header().success() || mrsp.messages_size() == 0) {
            return false;
        }
        // 反向：消息体里的字段需要被映射到 MessagePreview。Message 服务迁移时
        // 会把 SyncMessagesRsp.messages 改为 chatnow.message.Message；这里
        // 假设 messages(0) 的字段集合包含 message_id / sender_id / type / preview。
        // 本期占位映射如下（具体字段名与 Message 迁移收口同步调整）：
        const auto& m = mrsp.messages(0);
        out.set_message_id(m.message_id());
        out.set_sender_id(m.sender_id());
        out.set_seq_id(m.seq_id());
        out.set_send_time_ms(m.send_time_ms());
        // preview 文本由 Message 服务侧生成；本期留空
        return true;
    }

    bool require_member_(const std::string& cid, const std::string& uid) {
        auto m = _mysql_member->select_self(cid, uid);
        return m && !m->is_quit();
    }

    MemberRole role_of_(const std::string& cid, const std::string& uid) {
        auto m = _mysql_member->select_self(cid, uid);
        if (!m || m->is_quit()) return MemberRole::NORMAL;
        return m->role();
    }

    void invalidate_members_cache_(const std::string& cid) {
        try { _members_cache->del(cid); }
        catch(std::exception &e) { LOG_ERROR("invalidate cache 失败 {}: {}", cid, e.what()); }
    }

    static std::string private_id_of_(const std::string& a, const std::string& b) {
        const std::string& lo = (a < b) ? a : b;
        const std::string& hi = (a < b) ? b : a;
        return std::string("p_") + lo + "_" + hi;
    }

    std::string avatar_url_of_(const std::string& file_id) const {
        return _cfg.public_url_prefix + "/group_avatar/" + file_id;
    }

    void fill_self_member_info_(const ConversationMember& m,
                                unsigned long max_seq,
                                ::chatnow::conversation::SelfMemberInfo* out)
    {
        out->set_role(static_cast<::chatnow::conversation::MemberRole>(m.role()));
        out->set_joined_at_ms(
            (m.join_time() - boost::posix_time::ptime(boost::gregorian::date(1970,1,1)))
                .total_milliseconds());
        out->set_is_muted(m.muted());
        out->set_is_pinned(m.is_pinned());
        if (m.is_pinned()) {
            out->set_pin_time_ms(
                (m.pin_time() - boost::posix_time::ptime(boost::gregorian::date(1970,1,1)))
                    .total_milliseconds());
        }
        out->set_is_visible(m.visible());
        out->set_last_read_seq(m.last_read_seq());
        out->set_unread_count(max_seq > m.last_read_seq() ? max_seq - m.last_read_seq() : 0);
        if (m.has_draft()) out->set_draft(m.draft());
    }
```

然后把 ListConversations 占位删掉，新实现：

```cpp
public:
    void ListConversations(::google::protobuf::RpcController* base_cntl,
                           const ::chatnow::conversation::ListConversationsReq* req,
                           ::chatnow::conversation::ListConversationsRsp* rsp,
                           ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 取我的活跃会话视图列表（按 pin_time/update_time 排序由 view 实现）
            auto views = _mysql_member->list_ordered_by_user(auth.user_id);

            // 2. 收集 cid 列表 / 待批 user_id（peer_user_id 用于 PRIVATE 名字 / avatar 兜底）
            std::vector<std::string> peer_uids;
            for (auto &v : views) {
                if (v.peer_user_id) peer_uids.push_back(*v.peer_user_id);
            }
            std::unordered_map<std::string, ::chatnow::common::UserInfo> peer_map;
            (void)fetch_user_infos_(cntl, req->request_id(), peer_uids, peer_map);

            // 3. 转 proto
            for (auto &v : views) {
                if (!v.visible) continue;          // 隐藏会话不返回
                if (v.status == ConversationStatus::DISMISSED) continue;
                auto* c = rsp->add_conversations();
                c->set_conversation_id(v.conversation_id);
                c->set_type(static_cast<::chatnow::conversation::ConversationType>(v.conversation_type));
                if (v.conversation_name) c->set_name(*v.conversation_name);
                else if (v.peer_user_id) {
                    auto it = peer_map.find(*v.peer_user_id);
                    if (it != peer_map.end()) c->set_name(it->second.nickname());
                }
                if (v.avatar_id) c->set_avatar_url(*v.avatar_id);
                c->set_created_at_ms(
                    (v.create_time - boost::posix_time::ptime(boost::gregorian::date(1970,1,1)))
                        .total_milliseconds());
                c->set_member_count(v.member_count);
                c->set_status(static_cast<::chatnow::conversation::ConversationStatus>(v.status));
                // last_message：fail-soft（Message 不可达就留空）
                ::chatnow::message::MessagePreview preview;
                if (fetch_last_message_(cntl, req->request_id(), v.conversation_id,
                                        v.last_read_seq, preview)) {
                    c->mutable_last_message()->CopyFrom(preview);
                }
                // self
                ConversationMember m;  // 用 view 字段拼一个临时对象
                m.user_id(auth.user_id);
                m.conversation_id(v.conversation_id);
                m.role(v.role);
                m.muted(v.muted);
                m.visible(v.visible);
                if (v.pin_time) m.pin_time(*v.pin_time);
                m.last_read_seq(v.last_read_seq);
                m.last_ack_seq(v.last_ack_seq);
                m.join_time(v.join_time);
                if (v.draft) m.draft(*v.draft);
                fill_self_member_info_(m, v.max_seq, c->mutable_self());
            }
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(static_cast<int32_t>(rsp->conversations_size()));
        });
    }
```

- [ ] **Step 2: 实现 GetConversation**

```cpp
    void GetConversation(::google::protobuf::RpcController* base_cntl,
                         const ::chatnow::conversation::GetConversationReq* req,
                         ::chatnow::conversation::GetConversationRsp* rsp,
                         ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto c = _mysql_conv->select(req->conversation_id());
            if(!c)
                throw ServiceError(::chatnow::error::kConversationNotFound,
                                   "conversation not found");
            if(!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(c->conversation_id());
            out->set_type(static_cast<::chatnow::conversation::ConversationType>(c->conversation_type()));
            out->set_name(c->conversation_name());
            out->set_avatar_url(c->avatar_id());
            if (!c->description().empty()) out->set_description(c->description());
            out->set_created_at_ms(
                (c->create_time() - boost::posix_time::ptime(boost::gregorian::date(1970,1,1)))
                    .total_milliseconds());
            out->set_member_count(c->member_count());
            out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c->status()));

            auto self = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (self) fill_self_member_info_(*self, c->max_seq(), out->mutable_self());
        });
    }
```

- [ ] **Step 3: 实现 ListMembers**

```cpp
    void ListMembers(::google::protobuf::RpcController* base_cntl,
                     const ::chatnow::conversation::ListMembersReq* req,
                     ::chatnow::conversation::ListMembersRsp* rsp,
                     ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if(!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            auto rows = _mysql_member->list_active_members(req->conversation_id());
            std::vector<std::string> uids;
            for (auto &m : rows) uids.push_back(m.user_id());

            std::unordered_map<std::string, ::chatnow::common::UserInfo> umap;
            (void)fetch_user_infos_(cntl, req->request_id(), uids, umap);

            for (auto &m : rows) {
                auto* item = rsp->add_members();
                auto it = umap.find(m.user_id());
                if (it != umap.end()) item->mutable_user_info()->CopyFrom(it->second);
                else                  item->mutable_user_info()->set_user_id(m.user_id());
                item->set_role(static_cast<::chatnow::conversation::MemberRole>(m.role()));
                item->set_join_time_ms(
                    (m.join_time() - boost::posix_time::ptime(boost::gregorian::date(1970,1,1)))
                        .total_milliseconds());
            }
            rsp->mutable_page()->set_has_more(false);
            rsp->mutable_page()->set_total_count(rows.size());
        });
    }
```

> 注：Step 3 假设 `ConversationMemberTable::list_active_members(cid)` 已存在（来自原 ChatSessionMemberTable rename）。如不存在，回 T6 加入：`std::vector<ConversationMember> list_active_members(const std::string& cid)`，body 同 `list_by_session` 模式（query::conversation_id == cid && query::is_quit == false）。

- [ ] **Step 4: 实现 SearchConversations**

```cpp
    void SearchConversations(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::conversation::SearchConversationsReq* req,
                             ::chatnow::conversation::SearchConversationsRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto cid_hits = _es_conv->search(req->search_key(), std::nullopt, 50);
            if (cid_hits.empty()) return;
            // 仅返回 caller 是成员的会话
            auto convs = _mysql_conv->select(cid_hits);
            for (auto &c : convs) {
                if (!require_member_(c.conversation_id(), auth.user_id)) continue;
                if (c.status() == ConversationStatus::DISMISSED)            continue;
                auto* out = rsp->add_conversations();
                out->set_conversation_id(c.conversation_id());
                out->set_type(static_cast<::chatnow::conversation::ConversationType>(c.conversation_type()));
                out->set_name(c.conversation_name());
                out->set_avatar_url(c.avatar_id());
                out->set_member_count(c.member_count());
                out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c.status()));
            }
        });
    }
```

- [ ] **Step 5: 实现 GetMemberIds（含 `__system__` 内部调用放行）**

```cpp
    void GetMemberIds(::google::protobuf::RpcController* base_cntl,
                      const ::chatnow::conversation::GetMemberIdsReq* req,
                      ::chatnow::conversation::GetMemberIdsRsp* rsp,
                      ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            // 1. 优先走 Redis 缓存
            auto cached = _members_cache->list(req->conversation_id());
            if (!cached.empty()) {
                if (auth.user_id != "__system__"
                    && cached.find(auth.user_id) == cached.end())
                    throw ServiceError(::chatnow::error::kConversationNotMember,
                                       "not a member");
                for (auto &uid : cached) rsp->add_member_ids(uid);
                return;
            }
            // 2. 缓存未命中：查 DB + warm
            auto rows = _mysql_member->list_active_members(req->conversation_id());
            std::unordered_set<std::string> uids;
            for (auto &m : rows) uids.insert(m.user_id());
            _members_cache->warm(req->conversation_id(), uids);

            if (auth.user_id != "__system__"
                && uids.find(auth.user_id) == uids.end())
                throw ServiceError(::chatnow::error::kConversationNotMember,
                                   "not a member");
            for (auto &uid : uids) rsp->add_member_ids(uid);
        });
    }
```

- [ ] **Step 6: Commit**

```bash
git add conversation/source/conversation_server.h
git commit -m "$(cat <<'EOF'
conversation: 实现 5 个读取类 RPC

ListConversations / GetConversation / ListMembers /
SearchConversations / GetMemberIds（GetMemberIds 内部调用放行
__system__；其它走成员校验）。fetch_last_message_ 调
MessageService.SyncMessages，Message 未迁则 fail-soft 留空。
EOF
)"
```

---

## Task 11: 实现 4 个会话生命周期 RPC

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 实现 CreateConversation**

```cpp
    void CreateConversation(::google::protobuf::RpcController* base_cntl,
                            const ::chatnow::conversation::CreateConversationReq* req,
                            ::chatnow::conversation::CreateConversationRsp* rsp,
                            ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            const auto type_p = req->type();
            using ::chatnow::conversation::ConversationType;
            if (type_p == ConversationType::PRIVATE && req->member_ids_size() != 1)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "private requires exactly 1 peer");
            if (type_p == ConversationType::GROUP && req->member_ids_size() == 0)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "group needs initial members");

            std::string cid;
            if (type_p == ConversationType::PRIVATE) {
                const auto& peer = req->member_ids(0);
                cid = private_id_of_(auth.user_id, peer);
                if (_mysql_conv->exists(cid)) {
                    rsp->mutable_conversation()->set_conversation_id(cid);
                    rsp->mutable_conversation()->set_type(type_p);
                    return;        // 幂等
                }
            } else {
                cid = std::string("g_") + chatnow::utils::Uuid::create();
            }

            ::chatnow::ConversationType ent_type =
                (type_p == ConversationType::PRIVATE) ? ::chatnow::ConversationType::PRIVATE
              : (type_p == ConversationType::GROUP)   ? ::chatnow::ConversationType::GROUP
              :                                          ::chatnow::ConversationType::CHANNEL;

            int member_total = 1 + req->member_ids_size();
            ::chatnow::Conversation ent(
                cid,
                req->has_name() ? req->name() : std::string(),
                ent_type,
                boost::posix_time::microsec_clock::universal_time(),
                member_total,
                ::chatnow::ConversationStatus::NORMAL);
            if (type_p == ConversationType::PRIVATE)
                ent.peer_user_id(req->member_ids(0));
            if (type_p == ConversationType::GROUP) ent.owner_id(auth.user_id);
            if (req->has_avatar_url())
                ent.avatar_id(avatar_url_of_(req->avatar_url()));
            if (req->has_description()) ent.description(req->description());

            if(!_mysql_conv->insert(ent))
                throw ServiceError(::chatnow::error::kSystemInternalError, "insert conversation failed");

            // 成员行
            auto now = boost::posix_time::microsec_clock::universal_time();
            ::chatnow::ConversationMember owner_row(
                cid, auth.user_id, false, true,
                (type_p == ConversationType::GROUP) ? ::chatnow::MemberRole::OWNER
                                                    : ::chatnow::MemberRole::NORMAL,
                now);
            _mysql_member->insert(owner_row);
            for (int i = 0; i < req->member_ids_size(); ++i) {
                ::chatnow::ConversationMember m_row(cid, req->member_ids(i),
                    false, true, ::chatnow::MemberRole::NORMAL, now);
                _mysql_member->insert(m_row);
            }
            invalidate_members_cache_(cid);
            (void)_es_conv->append_data(ent);

            // 回填响应
            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(cid);
            out->set_type(type_p);
            out->set_name(ent.conversation_name());
            out->set_avatar_url(ent.avatar_id());
            out->set_member_count(member_total);
            out->set_status(::chatnow::conversation::ConversationStatus::CONVERSATION_NORMAL);
        });
    }
```

- [ ] **Step 2: 实现 UpdateConversation**

```cpp
    void UpdateConversation(::google::protobuf::RpcController* base_cntl,
                            const ::chatnow::conversation::UpdateConversationReq* req,
                            ::chatnow::conversation::UpdateConversationRsp* rsp,
                            ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");
            auto c = _mysql_conv->select(req->conversation_id());
            if (!c) throw ServiceError(::chatnow::error::kConversationNotFound, "not found");
            if (req->has_name())          c->conversation_name(req->name());
            if (req->has_avatar_url())    c->avatar_id(avatar_url_of_(req->avatar_url()));
            if (req->has_description())   c->description(req->description());
            if (req->has_announcement())  c->announcement(req->announcement());
            if(!_mysql_conv->update(c))
                throw ServiceError(::chatnow::error::kSystemInternalError, "update failed");
            (void)_es_conv->append_data(*c);
            // 回填
            auto* out = rsp->mutable_conversation();
            out->set_conversation_id(c->conversation_id());
            out->set_name(c->conversation_name());
            out->set_avatar_url(c->avatar_id());
            if (!c->description().empty()) out->set_description(c->description());
            out->set_member_count(c->member_count());
            out->set_status(static_cast<::chatnow::conversation::ConversationStatus>(c->status()));
        });
    }
```

- [ ] **Step 3: 实现 DismissConversation**

```cpp
    void DismissConversation(::google::protobuf::RpcController* base_cntl,
                             const ::chatnow::conversation::DismissConversationReq* req,
                             ::chatnow::conversation::DismissConversationRsp* rsp,
                             ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            if(!_mysql_conv->update_status(req->conversation_id(),
                                           ::chatnow::ConversationStatus::DISMISSED))
                throw ServiceError(::chatnow::error::kSystemInternalError, "update_status failed");
            (void)_es_conv->remove(req->conversation_id());
            invalidate_members_cache_(req->conversation_id());
            // 推送 CONVERSATION_DISMISSED_NOTIFY 留待 Push 接入；本期 fail-soft 不推
        });
    }
```

- [ ] **Step 4: 实现 QuitConversation**

```cpp
    void QuitConversation(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::conversation::QuitConversationReq* req,
                          ::chatnow::conversation::QuitConversationRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role == ::chatnow::MemberRole::NORMAL || role == ::chatnow::MemberRole::ADMIN) {
                if(!_mysql_member->soft_delete(req->conversation_id(), auth.user_id))
                    throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
                // member_count--（若 DAO 提供 dec；否则 select+update）
                auto c = _mysql_conv->select(req->conversation_id());
                if (c) {
                    c->member_count(std::max(0, c->member_count() - 1));
                    _mysql_conv->update(c);
                }
                invalidate_members_cache_(req->conversation_id());
            } else if (role == ::chatnow::MemberRole::OWNER) {
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner must transfer first");
            } else {
                throw ServiceError(::chatnow::error::kConversationNotMember, "not a member");
            }
        });
    }
```

> Step 4 假设 `ConversationMemberTable::soft_delete(cid, uid)` 已存在（rename 自原 `ChatSessionMemberTable::quit_session` 等）。如未存在，T6 已经从原文件 rename，方法名沿用原命名。如方法名不同，调整调用对应名字（如 `quit`、`mark_quit`、`soft_remove`）。

- [ ] **Step 5: Commit**

```bash
git add conversation/source/conversation_server.h
git commit -m "$(cat <<'EOF'
conversation: 实现 4 个会话生命周期 RPC

CreateConversation（PRIVATE 幂等 hash / GROUP 雪花）/
UpdateConversation（avatar_file_id→URL）/ DismissConversation（status 软删 + ES remove）/
QuitConversation（OWNER 必须先 TransferOwner）。
EOF
)"
```

---

## Task 12: 实现 4 个成员管理 RPC

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 实现 AddMembers**

```cpp
    void AddMembers(::google::protobuf::RpcController* base_cntl,
                    const ::chatnow::conversation::AddMembersReq* req,
                    ::chatnow::conversation::AddMembersRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto c = _mysql_conv->select(req->conversation_id());
            if (!c) throw ServiceError(::chatnow::error::kConversationNotFound, "not found");
            if (c->conversation_type() != ::chatnow::ConversationType::GROUP)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "only GROUP allows AddMembers");
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");

            constexpr int kGroupMemberLimit = 500;
            if (c->member_count() + req->member_ids_size() > kGroupMemberLimit)
                throw ServiceError(::chatnow::error::kConversationMemberLimit, "member limit");

            auto now = boost::posix_time::microsec_clock::universal_time();
            int added = 0;
            for (int i = 0; i < req->member_ids_size(); ++i) {
                const auto& uid = req->member_ids(i);
                auto m = _mysql_member->select_self(req->conversation_id(), uid);
                if (m && !m->is_quit()) continue;     // 已是成员
                if (m && m->is_quit()) {
                    // 二次入群：UPDATE 现有行
                    m->is_quit(false);
                    m->join_time(now);
                    m->role(::chatnow::MemberRole::NORMAL);
                    _mysql_member->update(m);
                } else {
                    ::chatnow::ConversationMember row(req->conversation_id(), uid,
                        false, true, ::chatnow::MemberRole::NORMAL, now);
                    _mysql_member->insert(row);
                }
                ++added;
            }
            if (added > 0) {
                c->member_count(c->member_count() + added);
                _mysql_conv->update(c);
                invalidate_members_cache_(req->conversation_id());
            }
        });
    }
```

> Step 1 假设 `ConversationMemberTable::update(shared_ptr<ConversationMember>)` 已存在。

- [ ] **Step 2: 实现 RemoveMembers**

```cpp
    void RemoveMembers(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::conversation::RemoveMembersReq* req,
                       ::chatnow::conversation::RemoveMembersRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto role = role_of_(req->conversation_id(), auth.user_id);
            if (role != ::chatnow::MemberRole::OWNER && role != ::chatnow::MemberRole::ADMIN)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "owner/admin only");
            int removed = 0;
            for (int i = 0; i < req->member_ids_size(); ++i) {
                const auto& uid = req->member_ids(i);
                if (uid == auth.user_id)
                    throw ServiceError(::chatnow::error::kSystemInvalidArgument, "cannot remove self");
                auto target = _mysql_member->select_self(req->conversation_id(), uid);
                if (!target || target->is_quit()) continue;
                if (target->role() == ::chatnow::MemberRole::OWNER)
                    throw ServiceError(::chatnow::error::kConversationNoPermission,
                                       "owner cannot be removed");
                _mysql_member->soft_delete(req->conversation_id(), uid);
                ++removed;
            }
            if (removed > 0) {
                auto c = _mysql_conv->select(req->conversation_id());
                if (c) {
                    c->member_count(std::max(0, c->member_count() - removed));
                    _mysql_conv->update(c);
                }
                invalidate_members_cache_(req->conversation_id());
            }
        });
    }
```

- [ ] **Step 3: 实现 TransferOwner**

```cpp
    void TransferOwner(::google::protobuf::RpcController* base_cntl,
                       const ::chatnow::conversation::TransferOwnerReq* req,
                       ::chatnow::conversation::TransferOwnerRsp* rsp,
                       ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            auto target = _mysql_member->select_self(req->conversation_id(), req->new_owner_id());
            if (!target || target->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "new owner must be a member");
            _mysql_member->update_role(req->conversation_id(), req->new_owner_id(),
                                       ::chatnow::MemberRole::OWNER);
            _mysql_member->update_role(req->conversation_id(), auth.user_id,
                                       ::chatnow::MemberRole::ADMIN);
            auto c = _mysql_conv->select(req->conversation_id());
            if (c) { c->owner_id(req->new_owner_id()); _mysql_conv->update(c); }
        });
    }
```

> Step 3 假设 `ConversationMemberTable::update_role(cid, uid, MemberRole)` 已存在（来自 chat_session_member 既有 update_role）。

- [ ] **Step 4: 实现 ChangeMemberRole**

```cpp
    void ChangeMemberRole(::google::protobuf::RpcController* base_cntl,
                          const ::chatnow::conversation::ChangeMemberRoleReq* req,
                          ::chatnow::conversation::ChangeMemberRoleRsp* rsp,
                          ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if (role_of_(req->conversation_id(), auth.user_id) != ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission, "owner only");
            using ::chatnow::conversation::MemberRole;
            if (req->role() == MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kSystemInvalidArgument,
                                   "use TransferOwner to assign OWNER");
            auto target = _mysql_member->select_self(req->conversation_id(), req->target_user_id());
            if (!target || target->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "target not in conversation");
            if (target->role() == ::chatnow::MemberRole::OWNER)
                throw ServiceError(::chatnow::error::kConversationNoPermission,
                                   "cannot change OWNER's role");
            _mysql_member->update_role(req->conversation_id(), req->target_user_id(),
                                       static_cast<::chatnow::MemberRole>(req->role()));
        });
    }
```

- [ ] **Step 5: Commit**

```bash
git add conversation/source/conversation_server.h
git commit -m "$(cat <<'EOF'
conversation: 实现 4 个成员管理 RPC

AddMembers（GROUP only / member_limit / 二次入群 UPDATE 现行）/
RemoveMembers（不能踢自己 / OWNER 不可被踢）/
TransferOwner（旧 OWNER 降 ADMIN）/
ChangeMemberRole（仅 OWNER；不能改 OWNER；不能升别人到 OWNER）。
EOF
)"
```

---

## Task 13: 实现 4 个自身偏好 RPC

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 实现 SetMute**

```cpp
    void SetMute(::google::protobuf::RpcController* base_cntl,
                 const ::chatnow::conversation::SetMuteReq* req,
                 ::chatnow::conversation::SetMuteRsp* rsp,
                 ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            m->muted(req->mute());
            _mysql_member->update(m);
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }
```

- [ ] **Step 2: 实现 SetPin**

```cpp
    void SetPin(::google::protobuf::RpcController* base_cntl,
                const ::chatnow::conversation::SetPinReq* req,
                ::chatnow::conversation::SetPinRsp* rsp,
                ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            if (req->pin())
                m->pin_time(boost::posix_time::microsec_clock::universal_time());
            else
                m->unpin();
            _mysql_member->update(m);
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }
```

- [ ] **Step 3: 实现 SetVisible**

```cpp
    void SetVisible(::google::protobuf::RpcController* base_cntl,
                    const ::chatnow::conversation::SetVisibleReq* req,
                    ::chatnow::conversation::SetVisibleRsp* rsp,
                    ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            if (!m || m->is_quit())
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            m->visible(req->visible());
            _mysql_member->update(m);
            auto c = _mysql_conv->select(req->conversation_id());
            fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }
```

- [ ] **Step 4: 实现 SaveDraft**

```cpp
    void SaveDraft(::google::protobuf::RpcController* base_cntl,
                   const ::chatnow::conversation::SaveDraftReq* req,
                   ::chatnow::conversation::SaveDraftRsp* rsp,
                   ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if(!_mysql_member->update_draft(req->conversation_id(), auth.user_id, req->draft()))
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            auto m = _mysql_member->select_self(req->conversation_id(), auth.user_id);
            auto c = _mysql_conv->select(req->conversation_id());
            if (m) fill_self_member_info_(*m, c ? c->max_seq() : 0, rsp->mutable_self());
        });
    }
```

- [ ] **Step 5: Commit**

```bash
git add conversation/source/conversation_server.h
git commit -m "conversation: 实现 4 个自身偏好 RPC（SetMute/SetPin/SetVisible/SaveDraft）"
```

---

## Task 14: 实现 MarkRead

**Files:**
- Modify: `conversation/source/conversation_server.h`

- [ ] **Step 1: 实现 MarkRead**

```cpp
    void MarkRead(::google::protobuf::RpcController* base_cntl,
                  const ::chatnow::conversation::MarkReadReq* req,
                  ::chatnow::conversation::MarkReadRsp* rsp,
                  ::google::protobuf::Closure* done) override
    {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            if(!require_member_(req->conversation_id(), auth.user_id))
                throw ServiceError(::chatnow::error::kConversationNotMember, "not member");
            (void)_mysql_member->update_last_read_seq(req->conversation_id(),
                                                     auth.user_id, req->last_read_seq());
            // GROUP READ_RECEIPT_NOTIFY 暂不推（YAGNI），PRIVATE 需要时由 Push 接入
        });
    }
```

- [ ] **Step 2: Commit**

```bash
git add conversation/source/conversation_server.h
git commit -m "conversation: 实现 MarkRead（防回退；GROUP READ_RECEIPT 暂不推）"
```

---

## Task 15: Gateway 16 个 chatsession HTTP handler 切到新 stub

**Files:**
- Modify: `gateway/source/gateway_server.h`

> 16 个 handler 改造模板一致：
>
> 1. `ChatSessionService_Stub stub(channel.get());` → `chatnow::conversation::ConversationService_Stub stub(channel.get());`
> 2. RPC 方法名按下表替换
> 3. `_relationship_service_name` → `_chatsession_service_name`（修 spec §11.6.3 列出的 3 处 pre-existing bug，本期一并修）
> 4. 报错路径与鉴权 helper 与 Identity / Relationship handler 对齐：用 `apply_auth_to_brpc(request, cntl, _auth)` 替代 `gateway_setup_trace`；写 `header.error_code/error_message` 而非 `set_success(false)/set_errmsg`

| Handler 函数名 | 旧 RPC | 新 RPC |
|---|---|---|
| `GetChatSessionList` | `GetChatSessionList` | `ListConversations` |
| `GetChatSessionDetail` | `GetChatSessionDetail` | `GetConversation` |
| `ChatSessionCreate` | `ChatSessionCreate` | `CreateConversation` |
| `SetChatSessionName` | `SetChatSessionName` | `UpdateConversation` |
| `SetChatSessionAvatar` | `SetChatSessionAvatar` | `UpdateConversation` |
| `AddChatSessionMember` | `AddChatSessionMember` | `AddMembers` |
| `RemoveChatSessionMember` | `RemoveChatSessionMember` | `RemoveMembers` |
| `TransferChatSessionOwner` | `TransferChatSessionOwner` | `TransferOwner` |
| `ModifyMemberPermission` | `ModifyMemberPermission` | `ChangeMemberRole` |
| `ModifyChatSessionStatus` | `ModifyChatSessionStatus` | `DismissConversation` (改语义：只支持 status=DISMISSED) |
| `GetChatSessionMember` | `GetChatSessionMember` | `ListMembers` |
| `SetSessionMuted` | `SetSessionMuted` | `SetMute` |
| `SetSessionPinned` | `SetSessionPinned` | `SetPin` |
| `SetSessionVisible` | `SetSessionVisible` | `SetVisible` |
| `QuitChatSession` | `QuitChatSession` | `QuitConversation` |
| `MsgReadAck` | `MsgReadAck` | `MarkRead` |
| `SearchChatSession` | `SearchChatSession` | `SearchConversations` |
| `GetMemberIdList` | `GetMemberIdList` | `GetMemberIds` |

> 注：Req/Rsp 类型需要从旧 `proto/chatsession.proto` 切到新 `chatnow::conversation::*Req/Rsp`。具体字段名映射（旧 chat_session_id → 新 conversation_id 等）由调用方客户端传参格式决定；本期保持 HTTP 路径与字段名不变（HTTP 兼容由 §8 保留），客户端继续传 `chat_session_id` 字段时，gateway handler 解析后**手动映射**到新 Req 的 `conversation_id`（即在 ParseFromString 之后、调 stub 之前，加 `new_req.set_conversation_id(old_req.chat_session_id());`）。
>
> 操作：以 `ChatSessionService_Stub stub(channel.get());` 为 anchor，**逐个 handler** 找到 18 行（含 3 行 `_relationship_service_name` 错挂为 chatsession 的 bug：约 998/1038/1078 行）做 4 步替换。建议每改 1 个 handler 跑一次 grep 确认进度。

- [ ] **Step 1: 修 3 处 pre-existing bug（_relationship_service_name → _chatsession_service_name）**

`gateway/source/gateway_server.h` 约 998 / 1038 / 1078 行：

```cpp
auto channel = _mm_channels->choose(_relationship_service_name);  // ← 错
```

改为：

```cpp
auto channel = _mm_channels->choose(_chatsession_service_name);   // ← 正确
```

之后整个 gateway 还会在 T16 把 `_chatsession_service_name` 字段名重命名为 `_conversation_service_name`，本 step 先把语义修对。

- [ ] **Step 2: 把 `_chatsession_service_name` 字段重命名为 `_conversation_service_name`，并把构造参数同步改名**

`gateway/source/gateway_server.h` 约 111 / 123 行（构造函数参数 + 成员初始化），以及所有引用 `_chatsession_service_name` 的位置：

```cpp
const std::string &chatsession_service_name,  // 旧
const std::string &conversation_service_name, // 新

_chatsession_service_name(chatsession_service_name),  // 旧
_conversation_service_name(conversation_service_name) // 新
```

把所有 `_chatsession_service_name` 出现处（`grep` 一遍确认）一并改成 `_conversation_service_name`。

- [ ] **Step 3: 所有 18 处 `ChatSessionService_Stub` → `chatnow::conversation::ConversationService_Stub`**

```cpp
// 旧
ChatSessionService_Stub stub(channel.get());
// 新
::chatnow::conversation::ConversationService_Stub stub(channel.get());
```

- [ ] **Step 4: 18 处 RPC 方法调用按上表替换**

逐个 handler 把 `stub.<旧名>(...)` 改为 `stub.<新名>(...)`，并把 Req / Rsp 类型从旧 `<OldName>Req/Rsp` 改为 `::chatnow::conversation::<NewName>Req/Rsp`，把 `req.set_user_id(_auth.user_id);` 删掉（user_id 走 metadata），把 `req.set_chat_session_id(...)` 调用改为 `req.set_conversation_id(httplib_old_param);`，把 `gateway_setup_trace(request, cntl)` 改为 `apply_auth_to_brpc(request, cntl, _auth)`，把 `rsp.set_success/set_errmsg` 改为 `rsp.mutable_header()->set_error_code(...) + set_error_message(...)`。

> 改造单个 handler 的标准模板（参考 Relationship 迁移已完成的 6 个 handler）：
>
> ```cpp
> void GetChatSessionList(const httplib::Request &request, httplib::Response &response) {
>     chatnow::gateway::LogContextScope _trace_scope;
>     ::chatnow::conversation::ListConversationsReq req;
>     ::chatnow::conversation::ListConversationsRsp rsp;
>     auto err_response = [&rsp, &response](const std::string &errmsg,
>                                           ::chatnow::error::ErrorCode code) {
>         rsp.mutable_header()->set_error_code(code);
>         rsp.mutable_header()->set_error_message(errmsg);
>         response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
>     };
>     if(!req.ParseFromString(request.body))
>         return err_response("反序列化失败", ::chatnow::error::kSystemInvalidArgument);
>     chatnow::gateway::AuthInfo _auth;
>     if (!chatnow::gateway::jwt_authenticate(request, response, _jwt_codec, _jwt_store,
>                                              /*whitelisted=*/false, _auth)) return;
>     auto channel = _mm_channels->choose(_conversation_service_name);
>     if(!channel) return err_response("会话子服务不可用", ::chatnow::error::kSystemUnavailable);
>     ::chatnow::conversation::ConversationService_Stub stub(channel.get());
>     brpc::Controller cntl;
>     chatnow::gateway::apply_auth_to_brpc(request, cntl, _auth);
>     stub.ListConversations(&cntl, &req, &rsp, nullptr);
>     if(cntl.Failed())
>         return err_response("会话子服务调用失败", ::chatnow::error::kSystemUnavailable);
>     response.set_content(rsp.SerializeAsString(), "application/x-protbuf");
> }
> ```

- [ ] **Step 5: 新增 2 个 handler（dismiss / save_draft）**

加 2 个 #define 路由：

```cpp
#define DISMISS_CHAT_SESSION  "/service/chatsession/dismiss_chat_session"
#define SAVE_CHAT_SESSION_DRAFT "/service/chatsession/save_draft"
```

handler 按上面模板各写一个，调 `DismissConversation` / `SaveDraft`。在路由注册位置（同其它 chatsession handler 注册的位置）追加 `_http_server->Post(...)` 行。

- [ ] **Step 6: 校验 grep 零命中**

```bash
grep -n "ChatSessionService_Stub\|_chatsession_service_name" gateway/source/gateway_server.h
```
Expected: 0 命中

- [ ] **Step 7: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "$(cat <<'EOF'
gateway: 16 chatsession handler 切到 ConversationService 新 stub + RPC

替换 stub / RPC 名 / Req-Rsp 类型 / 鉴权 helper / 错误响应模式。
顺手修 spec §11.6.3 提到的 3 处 _relationship_service_name 错挂 bug。
新增 dismiss / save_draft 两个 handler。HTTP 路径前缀保留 /service/chatsession/*。
EOF
)"
```

---

## Task 16: docker-compose + Transmite stub + Message 引用

**Files:**
- Modify: `docker-compose.yml`
- Modify: `transmite/source/transmite_server.h`
- Modify: `message/source/message_server.h`

- [ ] **Step 1: docker-compose 服务 rename**

打开 `docker-compose.yml`，把 `chatsession_server` 整段（service block + image + depends_on 别处对它的引用）rename 为 `conversation_server`。具体行号视当前文件而定，操作上：

```bash
sed -i 's/chatsession_server/conversation_server/g; s/chatsession\.log/conversation.log/g' docker-compose.yml
```

通读一遍确认没误改其它文本（如 `chatsession_service_name` 命令行参数也会被改 — 但 conf 已经改过命名一致；如果出现这个名字以参数形式存在需要保留，则手工 revert）。

- [ ] **Step 2: Transmite stub 名 rename（仅最小变更，本期不动业务）**

打开 `transmite/source/transmite_server.h`：

```cpp
// 旧
ChatSessionService_Stub session_stub(session_channel.get());
session_stub.GetMemberIdList(...);
// 新
::chatnow::conversation::ConversationService_Stub session_stub(session_channel.get());
session_stub.GetMemberIds(...);
```

把所有 `chatsession_service_name` 字段重命名为 `conversation_service_name`（约 6 行）：

```bash
sed -i 's/_chatsession_service_name/_conversation_service_name/g; s/chatsession_service_name/conversation_service_name/g' transmite/source/transmite_server.h
```

把 RPC 名 `GetMemberIdList` → `GetMemberIds`、Req/Rsp 类型从旧 `GetMemberIdListReq` → `::chatnow::conversation::GetMemberIdsReq`（同 Rsp）。

> 注：Transmite 服务自身的迁移留给后续 spec；这里只把对 Conversation 的调用更新到新 RPC 名 / 类型。

- [ ] **Step 3: Message 服务 ChatSessionMemberTable include 切换**

打开 `message/source/message_server.h`：

```cpp
// 旧
#include "dao/mysql_chat_session_member.hpp"
// 新
#include "dao/mysql_conversation_member.hpp"
```

把 `ChatSessionMemberTable::ptr` 替换为 `ConversationMemberTable::ptr`（仅 typedef 别名），把 `std::make_shared<ChatSessionMemberTable>(...)` 改为 `std::make_shared<ConversationMemberTable>(...)`。其它业务（Message 的 RPC 实现）保持不变。

- [ ] **Step 4: Commit**

```bash
git add docker-compose.yml transmite/source/transmite_server.h message/source/message_server.h
git commit -m "$(cat <<'EOF'
infra: docker-compose chatsession→conversation；transmite/message 引用同步

docker-compose service 名 rename。Transmite 对 Conversation 的 stub
调用改为新 stub + GetMemberIds（其它 transmite 业务下期迁）。
Message 服务把 ChatSessionMemberTable include 切到 ConversationMemberTable
（仅 typedef，业务不动；Message 完整迁移留后续 spec）。
EOF
)"
```

---

## Task 17: 删除旧 chatsession 资产

**Files:**
- Delete: `chatsession/`、`conf/chatsession_server.conf`、`odb/chat_session.hxx`、`odb/chat_session_member.hxx`、`odb/chat_session_view.hxx`、`common/dao/mysql_chat_session.hpp`、`common/dao/mysql_chat_session_member.hpp`、`proto/chatsession.proto`

- [ ] **Step 1: 全仓 grep 旧 stub / 旧类零命中**

```bash
grep -rn "ChatSessionService\|ChatSessionTable\|ChatSessionMemberTable\|ChatSessionMember[^_]\|class ChatSession " . --include="*.h" --include="*.hpp" --include="*.cc" --include="*.cxx" --include="*.hxx" 2>&1 | grep -v "build/\|third_party/\|\.pb\.h\|\.pb\.cc\|chatsession/" | head -20
```
Expected: 0 lines（chatsession/ 目录本身待删，过滤掉它）

如有命中，回到对应 task 修复。

- [ ] **Step 2: 删除目录与文件**

```bash
git rm -r chatsession/
git rm conf/chatsession_server.conf
git rm odb/chat_session.hxx odb/chat_session_member.hxx odb/chat_session_view.hxx
git rm common/dao/mysql_chat_session.hpp common/dao/mysql_chat_session_member.hpp
```

- [ ] **Step 3: 检查 proto/chatsession.proto 引用**

```bash
grep -rn "chatsession.pb\|proto/chatsession\.proto\|chatsession\.proto" . --include="*.h" --include="*.hpp" --include="*.cc" --include="*.cxx" --include="*.txt" --include="CMakeLists.txt" 2>&1 | grep -v "build/\|third_party/" | head -10
```
Expected: 0 命中

如 0 命中：
```bash
git rm proto/chatsession.proto
```

如有命中，记录并视情况一并修；若无法本期清理则保留 `proto/chatsession.proto` 至下期再删。

- [ ] **Step 4: 顶层 CMakeLists 移除 chatsession**

打开 `CMakeLists.txt`（根），如还有 `add_subdirectory(chatsession)` 一行，删除。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
cleanup: 删除旧 chatsession/ 目录 + ODB chat_session*.hxx + 旧 DAO + 旧 proto

整目录 rename 至 conversation/ 完成。proto/chatsession.proto 在
gateway/transmite/message 引用全部切到新 stub 后一并删除（若本步 grep
有残留则保留至后续 spec）。
EOF
)"
```

---

## Task 18: 在 spec 写入 §11 实施记录

**Files:**
- Modify: `docs/superpowers/specs/2026-05-16-conversation-service-migration-design.md`

- [ ] **Step 1: 把 §11 占位填上真实状态**

打开 spec 文件，把 §11 的 7 个小节按 Relationship spec §11 的格式写实：状态总览表、commit 序列（按时间序拷贝 git log --oneline 的 18 条）、横切 hotfix（若 T15 之外触发了任何 hotfix 单独列）、关键设计选择（PRIVATE 幂等 hash / avatar_file_id→URL / fail-soft last_message / __system__ 放行）、未完成项（编译验证 + 集成测试，依赖 Message 迁移）、给下一位 Agent 的接手清单（最优先：等 Message 迁移落地后整体编译；第二优先：跑端到端验收）、结构性约束（一表一 DAO；不动其它服务；本次不要求编译验证）。

> §11 的内容在执行 plan 过程中边干边记笔记积累，到 T18 一次性整理写入。

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-05-16-conversation-service-migration-design.md
git commit -m "docs(spec): conversation 迁移 spec 加 §11 实施记录"
```

---

## 验收检查（每项对应 spec §7 一条）

- [ ] **§7-1**：`grep -nE "optional string user_id|optional string session_id" proto/conversation/conversation_service.proto` 零命中；`grep -n "cc_generic_services" proto/conversation/conversation_service.proto` 命中 1 行
- [ ] **§7-2**：代码完成（编译验证待 Message 迁移，本期不要求过 build）
- [ ] **§7-3**：`grep -c "HANDLE_RPC" conversation/source/conversation_server.h` ≥ 18
- [ ] **§7-4**：DismissConversation 把 status 置 DISMISSED；ListConversations 不再返回该会话（代码层 review；运行时验证待 Message 迁移）
- [ ] **§7-5**：SaveDraft 写入 _draft；下一次 ListConversations 在 SelfMemberInfo.draft 返回（代码层 review）
- [ ] **§7-6**：MarkRead 仅 new_seq > old 时更新（DAO `update_last_read_seq` 含 `if(m->last_read_seq() < new_seq)` 守卫）
- [ ] **§7-7**：TransferOwner 后旧 OWNER 降 ADMIN；新 OWNER 必须先是成员（代码层 review）
- [ ] **§7-8**：CreateConversation(PRIVATE) 同两个 user 重复调用返回相同 conversation_id（`private_id_of_(a,b)` 输入对称 → 输出一致；`exists` 检查命中即直接返回 cid）
- [ ] **§7-9**：`grep -n "ChatSessionService_Stub" gateway/source/gateway_server.h` 零命中
- [ ] **§7-10**：`ls chatsession/` 失败；`ls proto/chatsession.proto` 失败；`grep -n "chatsession_server" CMakeLists.txt` 零命中

---

## 失败 / 回滚

- proto 字段删除是破坏性变更，但客户端继续传 user_id 字段不影响（proto3 unknown field 自动丢弃）
- ODB 表名变更：开发阶段 `docker-compose down -v` 重建。生产暂未上线，无 ALTER TABLE 风险
- Conversation 服务编译阻塞：与 Relationship 同模式（写新 stub，等 Message 迁移期统一过）。在 spec §11 实施记录中明确记录"Message 服务未迁导致 conversation_server 与 gateway_server 都无法编译，此为预期阻塞"
- 任何 step 中如发现 DAO 方法名与 chat_session_member 旧 DAO 不一致（如 `list_active_members` / `soft_delete` 命名差异），优先回到 T6 给 ConversationMemberTable 加缺失方法（保留旧方法名作为 alias），再继续后续 task
