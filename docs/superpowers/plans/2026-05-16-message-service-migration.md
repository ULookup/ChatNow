# Message 服务迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把旧 `message/` 服务（`MessageServiceImpl : public chatnow::MsgStorageService`）整体迁到新 proto `chatnow::message::MessageService`，落实 15 个 RPC（含 10 个新增），同时承担 push/notify proto 强类型化与 SeqGen 启动回填。

**Architecture:** 沿用 `2026-05-13-es-dual-write-redesign.md` 已落地的 DB→ES 派生事件 + Push 链路 fail-soft 拓扑（不重构 MQ）。proto 全套清理（4 个文件）+ 新建 2 个 DAO（reaction/pin）+ 现有 DAO 加 7 个方法 + MessageServiceImpl 完整重写 + Gateway 16 个 handler 调整。权限校验直接读 `conversation_member` 表（不走 Conversation RPC，避免高频路径双跳）。

**Tech Stack:** C++17, brpc, protobuf, ODB+MySQL, RabbitMQ (AMQP-CPP), Elasticsearch (elasticlient), Redis (redis-plus-plus), spdlog, GoogleTest（DAO/单元）。

**Spec:** `docs/superpowers/specs/2026-05-16-message-service-migration-design.md`

**前置约束（用户在 brainstorming 中明确）：**
- 客户端会重构，不考虑旧客户端兼容
- 开发阶段，DB 表变更可以 `docker-compose down -v` 重建，无需 ALTER TABLE
- 一表一 DAO 文件
- 不为了过编译就修改其它服务的 proto（Transmite / Push 阻塞编译留给各自迁移期）
- 本期 T1-T18 不跑 build（与 Relationship / Conversation 同模式）

---

## File Structure

### 新增文件

| 路径 | 责任 |
|---|---|
| `common/dao/mysql_message_reaction.hpp` | MessageReactionTable DAO：insert/remove/select_by_message/select_by_messages |
| `common/dao/mysql_message_pin.hpp` | MessagePinTable DAO：insert/remove/count_by_conversation/list_by_conversation/list_pinned_in |

### 修改文件

| 路径 | 改动 |
|---|---|
| `proto/message/message_service.proto` | 删 user_id/session_id（2 处）+ cc_generic_services + 全限定 |
| `proto/message/message_internal.proto` | cc_generic_services + 全限定 |
| `proto/push/notify.proto` | 加 3 NotifyType + 3 NotifyXxx + oneof 分支 + cc_generic_services + 全限定 |
| `proto/push/push_service.proto` | bytes notify_payload → NotifyMessage notify + cc_generic_services + 全限定 |
| `common/error/error_codes.hpp` | 新增 4001-4004 message 段错误码 |
| `common/dao/mysql_message.hpp` | +4 方法（update_status_to_recalled / select_max_seq_by_conversation / select_history / select_after） |
| `common/dao/mysql_user_timeline.hpp` | +3 方法（delete_by_message_ids / delete_by_conversation / select_max_user_seq_per_user） |
| `message/source/message_server.h` | **完整重写**：MessageServiceImpl 15 RPC + Builder + SeqGen 回填 + 跨服务 stub 改包 |
| `message/source/message_server.cc` | main：服务名 / etcd 路径同步改 |
| `message/CMakeLists.txt` | proto_files 加 message_service / message_internal / message_types / push notify / push_service / identity / media |
| `conf/message_server.conf` | gflag：服务名 message_service；下游 service 名同步 |
| `gateway/source/gateway_server.h` | 5 旧 message handler 切 stub + RPC；删 GetUnreadCount / GetOfflineMsg；新增 11 handler |
| `docker-compose.yml` | message_server 服务名不变；gflag 同步；depends_on 含 etcd / mysql / redis / rabbitmq / elasticsearch |

### 删除文件

| 路径 | 时机 |
|---|---|
| `proto/message.proto` 旧 flat proto（如还存在） | T17 cleanup（grep `chatnow::MsgStorage` 零命中后） |

---

## Task 1: proto/message/message_service.proto 清理 + cc_generic_services + 全限定

**Files:**
- Modify: `proto/message/message_service.proto`

**变更点：**
- 加 `option cc_generic_services = true;`
- import `common/envelope.proto` / `common/types.proto`（UserInfo 用）/ `message/message_types.proto`
- 引用全限定：`ResponseHeader` → `chatnow.common.ResponseHeader`；`Message` / `MessagePreview` / `ReactionGroup` → `chatnow.message.*`
- 删 `SelectByClientMsgIdReq.user_id` 字段（line 141）
- 删 `UpdateReadAckReq.user_id` 字段（line 151）

- [ ] **Step 1: 编辑 proto/message/message_service.proto**

```protobuf
syntax = "proto3";
package chatnow.message;

option cc_generic_services = true;

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
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.Message messages = 2;
    bool has_more = 3;
    uint64 latest_seq = 4;
}

message GetHistoryReq {
    string request_id = 1;
    string conversation_id = 2;
    uint64 before_seq = 3;
    int32 limit = 4;
}
message GetHistoryRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.Message messages = 2;
    bool has_more = 3;
}

message GetMessagesByIdReq {
    string request_id = 1;
    repeated int64 message_ids = 2;
}
message GetMessagesByIdRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.Message messages = 2;
}

message SearchMessagesReq {
    string request_id = 1;
    string conversation_id = 2;
    string keyword = 3;
    int32 limit = 4;
    string cursor = 5;
}
message SearchMessagesRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.Message messages = 2;
    bool has_more = 3;
    string next_cursor = 4;
}

message RecallMessageReq {
    string request_id = 1;
    string conversation_id = 2;
    int64 message_id = 3;
}
message RecallMessageRsp { chatnow.common.ResponseHeader header = 1; }

message AddReactionReq {
    string request_id = 1;
    int64 message_id = 2;
    string emoji = 3;
}
message AddReactionRsp { chatnow.common.ResponseHeader header = 1; }

message RemoveReactionReq {
    string request_id = 1;
    int64 message_id = 2;
    string emoji = 3;
}
message RemoveReactionRsp { chatnow.common.ResponseHeader header = 1; }

message GetReactionsReq {
    string request_id = 1;
    int64 message_id = 2;
}
message GetReactionsRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.ReactionGroup reactions = 2;
}

message PinMessageReq {
    string request_id = 1;
    string conversation_id = 2;
    int64 message_id = 3;
}
message PinMessageRsp { chatnow.common.ResponseHeader header = 1; }

message UnpinMessageReq {
    string request_id = 1;
    string conversation_id = 2;
    int64 message_id = 3;
}
message UnpinMessageRsp { chatnow.common.ResponseHeader header = 1; }

message ListPinnedReq {
    string request_id = 1;
    string conversation_id = 2;
}
message ListPinnedRsp {
    chatnow.common.ResponseHeader header = 1;
    repeated chatnow.message.Message messages = 2;
}

message DeleteMessagesReq {
    string request_id = 1;
    string conversation_id = 2;
    repeated int64 message_ids = 3;
}
message DeleteMessagesRsp { chatnow.common.ResponseHeader header = 1; }

message ClearConversationReq {
    string request_id = 1;
    string conversation_id = 2;
}
message ClearConversationRsp { chatnow.common.ResponseHeader header = 1; }

message SelectByClientMsgIdReq {
    string request_id = 1;
    string client_msg_id = 2;
}
message SelectByClientMsgIdRsp {
    chatnow.common.ResponseHeader header = 1;
    chatnow.message.Message message = 2;
}

message UpdateReadAckReq {
    string request_id = 1;
    string conversation_id = 2;
    uint64 seq_id = 3;
}
message UpdateReadAckRsp { chatnow.common.ResponseHeader header = 1; }
```

- [ ] **Step 2: 验证 grep 零命中**

Run: `grep -E "optional string user_id|optional string session_id" proto/message/message_service.proto`
Expected: 无输出（exit 1）

Run: `grep "cc_generic_services" proto/message/message_service.proto`
Expected: 1 行命中

- [ ] **Step 3: Commit**

```bash
git add proto/message/message_service.proto
git commit -m "proto(message): 删鉴权字段 + 全限定 + cc_generic_services"
```

---

## Task 2: proto/message/message_internal.proto + cc_generic_services + 全限定

**Files:**
- Modify: `proto/message/message_internal.proto`

变更点：保持 `chatnow.message.internal` 包名 + 加 `cc_generic_services` + 引用全限定。

- [ ] **Step 1: 编辑 proto/message/message_internal.proto**

```protobuf
syntax = "proto3";
package chatnow.message.internal;

option cc_generic_services = true;

import "message/message_types.proto";

message UserSeqPair {
    string user_id = 1;
    uint64 user_seq = 2;
}

message InternalMessage {
    chatnow.message.Message message = 1;
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
    chatnow.message.MessageType message_type = 7;
}
```

- [ ] **Step 2: Commit**

```bash
git add proto/message/message_internal.proto
git commit -m "proto(message-internal): 全限定 + cc_generic_services"
```

---

## Task 3: proto/push/notify.proto 加 3 个新 NotifyType + NotifyXxx + oneof 分支 + cc_generic_services

**Files:**
- Modify: `proto/push/notify.proto`

- [ ] **Step 1: 编辑 proto/push/notify.proto**

```protobuf
syntax = "proto3";
package chatnow.push;

option cc_generic_services = true;

import "common/types.proto";
import "message/message_types.proto";

enum NotifyType {
    FRIEND_ADD_APPLY_NOTIFY = 0;
    FRIEND_ADD_PROCESS_NOTIFY = 1;
    CONVERSATION_CREATE_NOTIFY = 2;
    CHAT_MESSAGE_NOTIFY = 3;
    FRIEND_REMOVE_NOTIFY = 4;
    MESSAGE_RECALLED_NOTIFY = 5;
    PRESENCE_CHANGE_NOTIFY = 6;
    TYPING_NOTIFY = 7;
    REACTION_CHANGED_NOTIFY = 8;
    PIN_CHANGED_NOTIFY = 9;
    READ_RECEIPT_NOTIFY = 10;
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

message NotifyFriendAddApply { chatnow.UserInfo user_info = 1; }
message NotifyFriendAddProcess { bool agree = 1; chatnow.UserInfo user_info = 2; }
message NotifyFriendRemove { string user_id = 1; }
message NotifyNewConversation { bytes conversation_payload = 1; }
message NotifyNewMessage { chatnow.message.Message message_info = 1; }
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

message NotifyReactionChanged {
    string conversation_id = 1;
    int64 message_id = 2;
    string actor_user_id = 3;
    string emoji = 4;
    bool added = 5;
}

message NotifyPinChanged {
    string conversation_id = 1;
    int64 message_id = 2;
    string actor_user_id = 3;
    bool is_pinned = 4;
}

message NotifyReadReceipt {
    string conversation_id = 1;
    string reader_user_id = 2;
    uint64 last_read_seq = 3;
}

message NotifyMessage {
    optional string notify_event_id = 1;
    NotifyType notify_type = 2;
    optional string trace_id = 14;
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
        NotifyReactionChanged reaction_changed = 15;
        NotifyPinChanged pin_changed = 16;
        NotifyReadReceipt read_receipt = 17;
    }
}
```

- [ ] **Step 2: 验证**

Run: `grep -E "REACTION_CHANGED_NOTIFY|PIN_CHANGED_NOTIFY|READ_RECEIPT_NOTIFY" proto/push/notify.proto`
Expected: 6 行（enum 3 + oneof 3 — 实际 reaction_changed/pin_changed/read_receipt 在 oneof，可能仅命中 enum 3 行）

Run: `grep "cc_generic_services" proto/push/notify.proto`
Expected: 1 行

- [ ] **Step 3: Commit**

```bash
git add proto/push/notify.proto
git commit -m "proto(push-notify): 加 3 NotifyType + cc_generic_services + 全限定"
```

---

## Task 4: proto/push/push_service.proto bytes → NotifyMessage 强类型

**Files:**
- Modify: `proto/push/push_service.proto`

- [ ] **Step 1: 编辑 proto/push/push_service.proto**

```protobuf
syntax = "proto3";
package chatnow.push;

option cc_generic_services = true;

import "common/envelope.proto";
import "push/notify.proto";

message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    chatnow.push.NotifyMessage notify = 3;
    optional uint64 user_seq = 4;
}
message PushToUserRsp {
    chatnow.common.ResponseHeader header = 1;
    int32 online_device_count = 2;
}

message PushBatchReq {
    string request_id = 1;
    repeated string user_id_list = 2;
    chatnow.push.NotifyMessage notify = 3;
    repeated UserSeqPair user_seqs = 4;
}
message PushBatchRsp {
    chatnow.common.ResponseHeader header = 1;
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

- [ ] **Step 2: 验证 bytes 字段已删**

Run: `grep "bytes notify_payload" proto/push/push_service.proto`
Expected: 无输出（exit 1）

- [ ] **Step 3: Commit**

```bash
git add proto/push/push_service.proto
git commit -m "proto(push-service): bytes notify_payload → NotifyMessage 强类型"
```

---

## Task 5: 在 error_codes.hpp 加 4001-4004 message 段错误码

**Files:**
- Modify: `common/error/error_codes.hpp:41`（在 3xxx 段之后、5xxx 段之前插入）

- [ ] **Step 1: 编辑 common/error/error_codes.hpp**

在 line 41 之后（kConversationMemberLimit 那一行后面），加：

```cpp
// 4000-4999 消息（与 proto/common/error.proto 同步）
inline constexpr int32_t kMessageNotFound          = 4001;
inline constexpr int32_t kMessageRecallTimeout     = 4002;
inline constexpr int32_t kMessageAlreadyRecalled   = 4003;
inline constexpr int32_t kMessageContentInvalid    = 4004;

```

- [ ] **Step 2: 验证**

Run: `grep -E "kMessage(NotFound|RecallTimeout|AlreadyRecalled|ContentInvalid)" common/error/error_codes.hpp`
Expected: 4 行命中

- [ ] **Step 3: Commit**

```bash
git add common/error/error_codes.hpp
git commit -m "error: 加 4000-4999 message 错误码常量"
```

---

## Task 6: mysql_message.hpp 加 4 个 DAO 方法

**Files:**
- Modify: `common/dao/mysql_message.hpp`

新增方法：
- `update_status_to_recalled(unsigned long mid)` — RecallMessage 软删
- `select_max_seq_by_conversation(const std::string& cid)` — SyncMessages latest_seq + SeqGen 回填用
- `select_history(...)` — GetHistory
- `select_after(...)` — SyncMessages

**注意：** ODB Message 实体中字段是 `_session_id`（legacy 命名，未做 rename），DAO 方法的参数名可用 `cid`，但内部 query 用 `query::session_id`（与现有 select_by_session 一致）。

- [ ] **Step 1: 在 MessageTable 类中加 4 个方法**

在 `select_by_client_msg` 方法之后插入：

```cpp
    /* brief: 把消息软删为 RECALLED 状态；status=0→1 race-safe；返回是否更新成功 */
    bool update_status_to_recalled(unsigned long mid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            using result = odb::result<Message>;
            result r(_db->query<Message>(query::message_id == mid &&
                                         query::status == MessageStatus::NORMAL));
            auto it = r.begin();
            if(it == r.end()) {
                trans.commit();
                return false;
            }
            Message m(*it);
            m.status(MessageStatus::REVOKED);
            m.content("");
            m.revoke_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(m);
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("update_status_to_recalled mid={} failed: {}", mid, e.what());
            return false;
        }
    }

    /* brief: 取会话内 max(seq_id)；SyncMessages latest_seq + SeqGen 回填 */
    unsigned long select_max_seq_by_conversation(const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            std::shared_ptr<Message> m(_db->query_one<Message>(
                (query::session_id == cid) + " ORDER BY " + query::seq_id + " DESC"));
            trans.commit();
            return m ? m->seq_id() : 0UL;
        } catch(std::exception &e) {
            LOG_ERROR("select_max_seq_by_conversation cid={} failed: {}", cid, e.what());
            return 0UL;
        }
    }

    /* brief: 取 [before_seq) 历史消息；按 seq_id DESC 排序，limit 条；过滤已删除 */
    std::vector<Message> select_history(const std::string &cid,
                                        unsigned long before_seq,
                                        int limit) {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            odb::result<Message> r(_db->query<Message>(
                (query::session_id == cid &&
                 query::seq_id < before_seq &&
                 query::status != MessageStatus::DELETED) +
                "ORDER BY " + query::seq_id + " DESC LIMIT " + std::to_string(limit)));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(*it);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("select_history cid={} before_seq={}: {}", cid, before_seq, e.what());
        }
        return res;
    }

    /* brief: 取 (after_seq, ...] 之后的消息；按 seq_id ASC 排序，limit 条；过滤已删除 */
    std::vector<Message> select_after(const std::string &cid,
                                      unsigned long after_seq,
                                      int limit) {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            odb::result<Message> r(_db->query<Message>(
                (query::session_id == cid &&
                 query::seq_id > after_seq &&
                 query::status != MessageStatus::DELETED) +
                "ORDER BY " + query::seq_id + " ASC LIMIT " + std::to_string(limit)));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(*it);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("select_after cid={} after_seq={}: {}", cid, after_seq, e.what());
        }
        return res;
    }
```

> **注意：** ODB MessageStatus 枚举值与 proto 不完全对齐。本仓 `odb/message.hxx` 中 `MessageStatus` 包含 `NORMAL=0 / REVOKED=1 / DELETED=2 / EDITED=3` 等。检查实际枚举名再使用（若与上述代码不一致，按实际改）。

- [ ] **Step 2: 验证**

Run: `grep -nE "update_status_to_recalled|select_max_seq_by_conversation|select_history|select_after" common/dao/mysql_message.hpp`
Expected: 4 处方法定义

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_message.hpp
git commit -m "dao(message): +4 方法（update_status_to_recalled/select_max_seq/select_history/select_after）"
```

---

## Task 7: mysql_user_timeline.hpp 加 3 个 DAO 方法

**Files:**
- Modify: `common/dao/mysql_user_timeline.hpp`

新增方法：
- `delete_by_message_ids(uid, cid, mids)` — DeleteMessages
- `delete_by_conversation(uid, cid)` — ClearConversation
- `select_max_user_seq_per_user()` — SeqGen 回填 user_seq

- [ ] **Step 1: 在 UserTimelineTable 类中加 3 个方法**

```cpp
    /* brief: 批量删除 user_timeline 中指定 message_id 的行（仅删调用方自己的） */
    int delete_by_message_ids(const std::string &uid,
                              const std::string &cid,
                              const std::vector<unsigned long> &mids) {
        if(mids.empty()) return 0;
        int n = 0;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            for(auto mid : mids) {
                n += static_cast<int>(_db->erase_query<UserTimeline>(
                    query::user_id == uid &&
                    query::session_id == cid &&
                    query::message_id == mid));
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("delete_by_message_ids uid={} cid={} size={}: {}",
                      uid, cid, mids.size(), e.what());
        }
        return n;
    }

    /* brief: 清空 user_timeline 中该会话所有行（仅删调用方自己的） */
    int delete_by_conversation(const std::string &uid, const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            int n = static_cast<int>(_db->erase_query<UserTimeline>(
                query::user_id == uid && query::session_id == cid));
            trans.commit();
            return n;
        } catch(std::exception &e) {
            LOG_ERROR("delete_by_conversation uid={} cid={}: {}", uid, cid, e.what());
            return 0;
        }
    }

    /* brief: 取所有用户的 max(user_seq)，给 SeqGen 启动回填用 */
    std::vector<std::pair<std::string, unsigned long>> select_max_user_seq_per_user() {
        std::vector<std::pair<std::string, unsigned long>> res;
        try {
            odb::transaction trans(_db->begin());
            using view = odb::query<UserTimeline>;
            odb::result<UserTimeline> r(_db->query<UserTimeline>(
                "GROUP BY " + view::user_id));
            // 简化方案：扫表两次（先 distinct user_id，再 per-user max）；千用户级别 OK
            std::set<std::string> uids;
            for(auto it = r.begin(); it != r.end(); ++it) uids.insert(it->user_id());
            for(const auto &u : uids) {
                std::shared_ptr<UserTimeline> m(_db->query_one<UserTimeline>(
                    (view::user_id == u) + " ORDER BY " + view::user_seq + " DESC"));
                if(m) res.emplace_back(u, m->user_seq());
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("select_max_user_seq_per_user: {}", e.what());
        }
        return res;
    }
```

> **如果性能不足**：可改为 ODB native query `SELECT user_id, MAX(user_seq) FROM user_timeline GROUP BY user_id`。本期 YAGNI（启动只跑一次）。

- [ ] **Step 2: 验证**

Run: `grep -nE "delete_by_message_ids|delete_by_conversation|select_max_user_seq_per_user" common/dao/mysql_user_timeline.hpp`
Expected: 3 处方法定义

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_user_timeline.hpp
git commit -m "dao(user_timeline): +3 方法（delete_by_message_ids/delete_by_conversation/select_max_user_seq）"
```

---

## Task 8: 新建 common/dao/mysql_message_reaction.hpp

**Files:**
- Create: `common/dao/mysql_message_reaction.hpp`

- [ ] **Step 1: 新建文件**

```cpp
#pragma once

/**
 * MessageReactionTable —— message_reaction 表 DAO
 * ---
 * 表已通过 odb/message_reaction.hxx + ODB 自动建表。本类仅封装 CRUD。
 *
 * 索引：
 *   uk_msg_user_emoji (message_id, user_id, emoji)  unique
 *   idx_msg            (message_id)
 *
 * 一表一 DAO 文件惯例（与 mysql_user_block.hpp 同模式）。
 */

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/result.hxx>
#include <odb/mysql/database.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "../infra/logger.hpp"
#include "odb/message_reaction.hxx"
#include "odb/message_reaction-odb.hxx"

namespace chatnow {

struct ReactionRow {
    unsigned long id;            // 自增主键，用于 "最近 N 个" 稳定排序
    unsigned long message_id;
    std::string user_id;
    std::string emoji;
};

class MessageReactionTable {
public:
    using ptr = std::shared_ptr<MessageReactionTable>;
    explicit MessageReactionTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 插入一条 reaction；唯一索引冲突视幂等成功 */
    bool insert(unsigned long mid, const std::string &uid, const std::string &emoji) {
        try {
            odb::transaction trans(_db->begin());
            MessageReaction r(mid, uid, emoji);
            r.create_time(boost::posix_time::microsec_clock::universal_time());
            _db->persist(r);
            trans.commit();
            return true;
        } catch(const odb::object_already_persistent &) {
            return true;
        } catch(const odb::database_exception &e) {
            // MySQL 唯一索引冲突错误码 1062
            std::string what = e.what();
            if(what.find("Duplicate") != std::string::npos ||
               what.find("1062") != std::string::npos) return true;
            LOG_ERROR("MessageReaction.insert mid={} uid={} emoji={}: {}", mid, uid, emoji, what);
            return false;
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.insert mid={} uid={} emoji={}: {}", mid, uid, emoji, e.what());
            return false;
        }
    }

    /* brief: 删除一条 reaction；不存在返回 true（幂等） */
    bool remove(unsigned long mid, const std::string &uid, const std::string &emoji) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            (void)_db->erase_query<MessageReaction>(
                query::message_id == mid &&
                query::user_id == uid &&
                query::emoji == emoji);
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.remove mid={} uid={} emoji={}: {}", mid, uid, emoji, e.what());
            return false;
        }
    }

    /* brief: 取单条消息的所有 reaction 行（给服务层 GROUP BY emoji 聚合） */
    std::vector<ReactionRow> select_by_message(unsigned long mid) {
        std::vector<ReactionRow> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            odb::result<MessageReaction> r(_db->query<MessageReaction>(
                (query::message_id == mid) + " ORDER BY id ASC"));
            for(auto it = r.begin(); it != r.end(); ++it) {
                res.push_back({0UL, it->message_id(), it->user_id(), it->emoji()});
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.select_by_message mid={}: {}", mid, e.what());
        }
        return res;
    }

    /* brief: 批量取多条消息的所有 reaction 行（GetHistory/SyncMessages 用） */
    std::vector<ReactionRow> select_by_messages(const std::vector<unsigned long> &mids) {
        std::vector<ReactionRow> res;
        if(mids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            odb::result<MessageReaction> r(_db->query<MessageReaction>(
                query::message_id.in_range(mids.begin(), mids.end()) +
                " ORDER BY message_id ASC, id ASC"));
            for(auto it = r.begin(); it != r.end(); ++it) {
                res.push_back({0UL, it->message_id(), it->user_id(), it->emoji()});
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.select_by_messages size={}: {}", mids.size(), e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
```

- [ ] **Step 2: 验证文件创建**

Run: `ls -la common/dao/mysql_message_reaction.hpp`
Expected: 文件存在

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_message_reaction.hpp
git commit -m "dao: 新增 MessageReactionTable（insert/remove/select 幂等）"
```

---

## Task 9: 新建 common/dao/mysql_message_pin.hpp

**Files:**
- Create: `common/dao/mysql_message_pin.hpp`

**注意：** odb/message_pin.hxx 实体使用 `_session_id` 字段（legacy 命名）。DAO 对外 API 用 `cid`，但内部 query 用 `query::session_id`，与 mysql_message.hpp 同模式。

- [ ] **Step 1: 新建文件**

```cpp
#pragma once

/**
 * MessagePinTable —— message_pin 表 DAO
 * ---
 * 唯一索引：uk_conv_msg (session_id, message_id)
 *
 * 注：odb/message_pin.hxx 内字段名仍是 _session_id（legacy），
 *    DAO 对外签名用 cid，内部 query 用 session_id 列名。
 */

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/result.hxx>
#include <odb/mysql/database.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "../infra/logger.hpp"
#include "odb/message_pin.hxx"
#include "odb/message_pin-odb.hxx"

namespace chatnow {

class MessagePinTable {
public:
    using ptr = std::shared_ptr<MessagePinTable>;
    explicit MessagePinTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 写一条 pin 行；唯一索引冲突视幂等成功 */
    bool insert(const std::string &cid, unsigned long mid, const std::string &pinner_uid) {
        try {
            odb::transaction trans(_db->begin());
            MessagePin p(cid, mid, pinner_uid);
            p.pinned_at(boost::posix_time::microsec_clock::universal_time());
            _db->persist(p);
            trans.commit();
            return true;
        } catch(const odb::object_already_persistent &) {
            return true;
        } catch(const odb::database_exception &e) {
            std::string what = e.what();
            if(what.find("Duplicate") != std::string::npos ||
               what.find("1062") != std::string::npos) return true;
            LOG_ERROR("MessagePin.insert cid={} mid={} by={}: {}", cid, mid, pinner_uid, what);
            return false;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.insert cid={} mid={} by={}: {}", cid, mid, pinner_uid, e.what());
            return false;
        }
    }

    bool remove(const std::string &cid, unsigned long mid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            (void)_db->erase_query<MessagePin>(
                query::session_id == cid && query::message_id == mid);
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.remove cid={} mid={}: {}", cid, mid, e.what());
            return false;
        }
    }

    int count_by_conversation(const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(query::session_id == cid));
            int n = 0;
            for(auto it = r.begin(); it != r.end(); ++it) ++n;
            trans.commit();
            return n;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.count_by_conversation cid={}: {}", cid, e.what());
            return 0;
        }
    }

    std::vector<unsigned long> list_by_conversation(const std::string &cid, int limit = 10) {
        std::vector<unsigned long> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(
                (query::session_id == cid) + " ORDER BY pinned_at DESC LIMIT " +
                std::to_string(limit)));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(it->message_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.list_by_conversation cid={}: {}", cid, e.what());
        }
        return res;
    }

    /* brief: 该 cid 的 mid 集合中已 pin 的子集，给 fill_pin_flag 批量用 */
    std::vector<unsigned long> list_pinned_in(const std::string &cid,
                                              const std::vector<unsigned long> &mids) {
        std::vector<unsigned long> res;
        if(mids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(
                query::session_id == cid &&
                query::message_id.in_range(mids.begin(), mids.end())));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(it->message_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.list_pinned_in cid={} size={}: {}", cid, mids.size(), e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
```

- [ ] **Step 2: 验证**

Run: `ls -la common/dao/mysql_message_pin.hpp`
Expected: 文件存在

- [ ] **Step 3: Commit**

```bash
git add common/dao/mysql_message_pin.hpp
git commit -m "dao: 新增 MessagePinTable（insert/remove/count/list 幂等）"
```

---

## Task 10: 重写 message_server.h 骨架（类切换 + Builder + SeqGen 回填）

**Files:**
- Modify: `message/source/message_server.h`（**整体重写骨架**，handler 业务逻辑分散在 T11–T15）

本任务只完成：

1. 新类 `class MessageServiceImpl : public chatnow::message::MessageService`
2. 注入字段：mysql_msg / mysql_user_timeline / mysql_member（取角色） / mysql_reaction / mysql_pin / es_msg / seq_gen / push_publisher / push_outbox / es_publisher / es_outbox / mm_channels（identity / media）/ cfg
3. 15 个 RPC 的空实现框架（`HANDLE_RPC` 包住，body 内仅一行 `throw ServiceError(kSystemInternalError, "not implemented");`）
4. `MessageServerBuilder` 复用现有 + 加 `backfill_seq_from_db_()`
5. 跨服务 stub 全部切到新包名

**handler 业务逻辑留 T11-T15。本任务保留旧的 onDBMessage / onESIndexMessage 现有实现，仅切包名 namespace。**

- [ ] **Step 1: 阅读现有 message_server.h 全文**

Run: `wc -l message/source/message_server.h`
Expected: ~1414 行

- [ ] **Step 2: 完整重写 message/source/message_server.h**

按以下骨架结构重写。**保留 onDBMessage / onESIndexMessage 现有业务逻辑**（仅切包名 + 字段访问到新 proto）。

```cpp
#pragma once

/**
 * MessageServiceImpl —— chatnow::message::MessageService 实现
 * ---
 * 见 docs/superpowers/specs/2026-05-16-message-service-migration-design.md
 */

#include <brpc/server.h>
#include <brpc/channel.h>
#include <brpc/closure_guard.h>
#include <google/protobuf/service.h>

#include "message/message_service.pb.h"
#include "message/message_internal.pb.h"
#include "message/message_types.pb.h"
#include "push/notify.pb.h"
#include "push/push_service.pb.h"
#include "identity/identity_service.pb.h"
#include "media/media_service.pb.h"

#include "common/auth/auth_context.hpp"
#include "common/auth/forward_auth.hpp"
#include "common/error/handle_rpc.hpp"
#include "common/error/service_error.hpp"
#include "common/error/error_codes.hpp"
#include "common/log/log_context.hpp"
#include "common/infra/logger.hpp"
#include "common/infra/etcd.hpp"
#include "common/infra/channels.hpp"
#include "common/dao/mysql_message.hpp"
#include "common/dao/mysql_user_timeline.hpp"
#include "common/dao/mysql_conversation_member.hpp"
#include "common/dao/mysql_message_reaction.hpp"
#include "common/dao/mysql_message_pin.hpp"
#include "common/dao/data_es.hpp"
#include "common/dao/data_redis.hpp"
#include "common/mq/rabbitmq.hpp"

namespace chatnow::message {

inline constexpr int64_t kRecallTimeoutMs = 120 * 1000;
inline constexpr int     kPinLimit        = 10;
inline constexpr int     kMaxLimit        = 100;
inline constexpr int     kMaxSearchLimit  = 50;
inline constexpr int     kMaxEmojiBytes   = 16;
inline constexpr const char *kSystemUserId = "__system__";

/* MessageServiceImpl 实现 chatnow::message::MessageService */
class MessageServiceImpl : public chatnow::message::MessageService {
public:
    MessageServiceImpl(const std::string &identity_service_name,
                       const std::string &media_service_name,
                       const ServiceManager::ptr &mm_channels,
                       const MessageTable::ptr &mysql_msg,
                       const UserTimelineTable::ptr &mysql_user_timeline,
                       const ConversationMemberTable::ptr &mysql_member,
                       const MessageReactionTable::ptr &mysql_reaction,
                       const MessagePinTable::ptr &mysql_pin,
                       const ESMessage::ptr &es_msg,
                       const SeqGen::ptr &seq_gen,
                       const Publisher::ptr &push_publisher,
                       const PushOutbox::ptr &push_outbox,
                       const Publisher::ptr &es_publisher,
                       const ESOutbox::ptr &es_outbox)
        : _identity_service_name(identity_service_name),
          _media_service_name(media_service_name),
          _mm_channels(mm_channels),
          _mysql_msg(mysql_msg),
          _mysql_user_timeline(mysql_user_timeline),
          _mysql_member(mysql_member),
          _mysql_reaction(mysql_reaction),
          _mysql_pin(mysql_pin),
          _es_msg(es_msg),
          _seq_gen(seq_gen),
          _push_publisher(push_publisher),
          _push_outbox(push_outbox),
          _es_publisher(es_publisher),
          _es_outbox(es_outbox) {}

    ~MessageServiceImpl() override = default;

    // ====== 15 个 RPC：T10 仅占位 throw kSystemInternalError；T11-T15 替换 ======

    void GetHistory(::google::protobuf::RpcController* base_cntl,
                    const GetHistoryReq* req, GetHistoryRsp* rsp,
                    ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "GetHistory not implemented");
        });
    }

    void SyncMessages(::google::protobuf::RpcController* base_cntl,
                      const SyncMessagesReq* req, SyncMessagesRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "SyncMessages not implemented");
        });
    }

    void GetMessagesById(::google::protobuf::RpcController* base_cntl,
                         const GetMessagesByIdReq* req, GetMessagesByIdRsp* rsp,
                         ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "GetMessagesById not implemented");
        });
    }

    void SearchMessages(::google::protobuf::RpcController* base_cntl,
                        const SearchMessagesReq* req, SearchMessagesRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "SearchMessages not implemented");
        });
    }

    void RecallMessage(::google::protobuf::RpcController* base_cntl,
                       const RecallMessageReq* req, RecallMessageRsp* rsp,
                       ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "RecallMessage not implemented");
        });
    }

    void AddReaction(::google::protobuf::RpcController* base_cntl,
                     const AddReactionReq* req, AddReactionRsp* rsp,
                     ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "AddReaction not implemented");
        });
    }

    void RemoveReaction(::google::protobuf::RpcController* base_cntl,
                        const RemoveReactionReq* req, RemoveReactionRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "RemoveReaction not implemented");
        });
    }

    void GetReactions(::google::protobuf::RpcController* base_cntl,
                      const GetReactionsReq* req, GetReactionsRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "GetReactions not implemented");
        });
    }

    void PinMessage(::google::protobuf::RpcController* base_cntl,
                    const PinMessageReq* req, PinMessageRsp* rsp,
                    ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "PinMessage not implemented");
        });
    }

    void UnpinMessage(::google::protobuf::RpcController* base_cntl,
                      const UnpinMessageReq* req, UnpinMessageRsp* rsp,
                      ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "UnpinMessage not implemented");
        });
    }

    void ListPinnedMessages(::google::protobuf::RpcController* base_cntl,
                            const ListPinnedReq* req, ListPinnedRsp* rsp,
                            ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "ListPinnedMessages not implemented");
        });
    }

    void DeleteMessages(::google::protobuf::RpcController* base_cntl,
                        const DeleteMessagesReq* req, DeleteMessagesRsp* rsp,
                        ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "DeleteMessages not implemented");
        });
    }

    void ClearConversation(::google::protobuf::RpcController* base_cntl,
                           const ClearConversationReq* req, ClearConversationRsp* rsp,
                           ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "ClearConversation not implemented");
        });
    }

    void SelectByClientMsgId(::google::protobuf::RpcController* base_cntl,
                             const SelectByClientMsgIdReq* req, SelectByClientMsgIdRsp* rsp,
                             ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "SelectByClientMsgId not implemented");
        });
    }

    void UpdateReadAck(::google::protobuf::RpcController* base_cntl,
                       const UpdateReadAckReq* req, UpdateReadAckRsp* rsp,
                       ::google::protobuf::Closure* done) override {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "UpdateReadAck not implemented");
        });
    }

    // ====== MQ consumer：保留现有业务逻辑，仅切包名 ======

    ConsumeAction onDBMessage(const char *body, size_t sz, bool redelivered) {
        // 实现：按现有逻辑（message_server.h:583-749 全文搬过来）
        // 关键改动：
        //   - chatnow::InternalMessage → chatnow::message::internal::InternalMessage
        //   - 字段访问：内部 InternalMessage 字段不变（message / member_id_list / user_seqs / is_large_group）
        //   - 投 push_queue 时构造 chatnow::push::NotifyMessage（强类型）替代旧 chatnow::NotifyMessage
        //   - publish 时 SerializeAsString() 一致
        //
        // **本 step 占位说明**：T18 完成本服务实施后，再做 onDBMessage 内部包名替换批量审视。
        //
        // 占位实现：返回 ACK 不做实际处理，避免阻塞 brpc 启动。
        (void)body; (void)sz; (void)redelivered;
        LOG_WARN("onDBMessage placeholder; will be replaced in T18");
        return ConsumeAction::Ack;
    }

    ConsumeAction onESIndexMessage(const char *body, size_t sz, bool redelivered) {
        (void)body; (void)sz; (void)redelivered;
        LOG_WARN("onESIndexMessage placeholder; will be replaced in T18");
        return ConsumeAction::Ack;
    }

private:
    std::string _identity_service_name;
    std::string _media_service_name;
    ServiceManager::ptr _mm_channels;

    MessageTable::ptr _mysql_msg;
    UserTimelineTable::ptr _mysql_user_timeline;
    ConversationMemberTable::ptr _mysql_member;
    MessageReactionTable::ptr _mysql_reaction;
    MessagePinTable::ptr _mysql_pin;

    ESMessage::ptr _es_msg;
    SeqGen::ptr _seq_gen;

    Publisher::ptr _push_publisher;
    PushOutbox::ptr _push_outbox;
    Publisher::ptr _es_publisher;
    ESOutbox::ptr _es_outbox;
};

// ===== Server 与 Builder =====

/* MessageServer：持有 brpc::Server + MQClient + reaper threads + Registry */
class MessageServer {
public:
    using ptr = std::shared_ptr<MessageServer>;
    MessageServer(const std::shared_ptr<brpc::Server> &server,
                  MessageServiceImpl *impl,
                  const Registry::ptr &registry,
                  const MQClient::ptr &mq_client)
        : _rpc_server(server), _service_impl(impl),
          _registry(registry), _mq_client(mq_client) {}

    ~MessageServer() {
        if(_rpc_server) { _rpc_server->Stop(0); _rpc_server->Join(); }
        _mq_client.reset();
        // brpc Server 析构 SERVER_OWNS_SERVICE → delete impl
    }

    void start() { _rpc_server->RunUntilAskedToQuit(); }

private:
    std::shared_ptr<brpc::Server> _rpc_server;
    MessageServiceImpl *_service_impl {nullptr};
    Registry::ptr _registry;
    MQClient::ptr _mq_client;
};

class MessageServerBuilder {
public:
    void make_mysql_object(const std::string &host, int port, const std::string &user,
                           const std::string &pwd, const std::string &db, int pool) {
        _odb_db = ODBFactory::create(user, pwd, host, port, db, pool);
        _mysql_msg = std::make_shared<MessageTable>(_odb_db);
        _mysql_user_timeline = std::make_shared<UserTimelineTable>(_odb_db);
        _mysql_member = std::make_shared<ConversationMemberTable>(_odb_db);
        _mysql_reaction = std::make_shared<MessageReactionTable>(_odb_db);
        _mysql_pin = std::make_shared<MessagePinTable>(_odb_db);
    }
    void make_redis_object(const std::string &host, int port, int db, bool keepalive) {
        _redis = RedisClientFactory::create(host, port, db, keepalive);
        _seq_gen = std::make_shared<SeqGen>(_redis);
        _push_outbox = std::make_shared<PushOutbox>(_redis);
        _es_outbox = std::make_shared<ESOutbox>(_redis);
    }
    void make_es_object(const std::vector<std::string> &hosts) {
        _es_client = ESClientFactory::create(hosts);
        _es_msg = std::make_shared<ESMessage>(_es_client);
    }
    void make_mq_object(const std::string &user, const std::string &pwd,
                        const std::string &host /* ... 沿用现有签名 ... */ ) {
        _mq_client = MQClientFactory::create(user, pwd, host);
    }
    void make_discovery_object(const std::string &reg_host,
                               const std::string &base_dir,
                               const std::string &identity_service_name,
                               const std::string &media_service_name) {
        _identity_service_name = identity_service_name;
        _media_service_name = media_service_name;
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(),
                                std::placeholders::_1, std::placeholders::_2);
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_identity_service_name);
        _mm_channels->declared(_media_service_name);
        _service_discovery = std::make_shared<Discovery>(reg_host, base_dir, put_cb, del_cb);
    }
    void make_registry_object(const std::string &reg_host,
                              const std::string &service_name,
                              const std::string &access_host) {
        _registry = std::make_shared<Registry>(reg_host);
        _registry->registry(service_name, access_host);
    }
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        MessageServiceImpl *impl = new MessageServiceImpl(
            _identity_service_name, _media_service_name, _mm_channels,
            _mysql_msg, _mysql_user_timeline, _mysql_member,
            _mysql_reaction, _mysql_pin, _es_msg, _seq_gen,
            /*push_publisher=*/nullptr, _push_outbox,
            /*es_publisher=*/nullptr, _es_outbox);
        _service_impl = impl;
        int ret = _rpc_server->AddService(impl, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) { LOG_ERROR("AddService failed"); abort(); }
        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        if(_rpc_server->Start(port, &options) != 0) { LOG_ERROR("brpc Start failed"); abort(); }
        backfill_seq_from_db_();
        // T18: subscribe MQ + start outbox reapers
    }

    MessageServer::ptr build() {
        return std::make_shared<MessageServer>(_rpc_server, _service_impl, _registry, _mq_client);
    }

private:
    /* SeqGen 启动回填：先回填，再订阅 MQ */
    void backfill_seq_from_db_() {
        // session-seq：每个 cid 拉 max(seq_id)
        // 简化方案：扫所有 conversation_id distinct（小规模），逐个 backfill
        // 实际查询走 mysql_msg->select_max_seq_by_conversation 的循环；
        // 大规模可在 mysql_message DAO 加 select_max_seq_per_conversation 聚合方法（YAGNI v1）
        LOG_INFO("seqgen backfill: TODO in T18");
    }

private:
    std::shared_ptr<odb::core::database> _odb_db;
    std::shared_ptr<sw::redis::Redis> _redis;
    std::shared_ptr<elasticlient::Client> _es_client;
    MQClient::ptr _mq_client;
    Registry::ptr _registry;
    Discovery::ptr _service_discovery;

    MessageTable::ptr _mysql_msg;
    UserTimelineTable::ptr _mysql_user_timeline;
    ConversationMemberTable::ptr _mysql_member;
    MessageReactionTable::ptr _mysql_reaction;
    MessagePinTable::ptr _mysql_pin;
    ESMessage::ptr _es_msg;
    SeqGen::ptr _seq_gen;
    PushOutbox::ptr _push_outbox;
    ESOutbox::ptr _es_outbox;

    ServiceManager::ptr _mm_channels;
    std::string _identity_service_name;
    std::string _media_service_name;

    std::shared_ptr<brpc::Server> _rpc_server;
    MessageServiceImpl *_service_impl {nullptr};
};

} // namespace chatnow::message
```

**说明**：
- 类与 Builder 内部各方法签名/字段以 Conversation 服务（`conversation/source/conversation_server.h`）的实际方法名为准；上面是结构示意。
- onDBMessage / onESIndexMessage 的真实业务逻辑迁移留 T18，本任务用 placeholder 占位避免编译断裂。
- backfill_seq_from_db_ 真实实现留 T18（需要 select_max_seq_per_conversation 聚合方法或循环；本任务先 LOG_INFO 占位）。

- [ ] **Step 3: 编辑 message/source/message_server.cc 的 main**

```cpp
#include "message_server.h"
#include <gflags/gflags.h>

DEFINE_int32(run_mode, 1, "运行模式 0=debug 1=release");
DEFINE_int32(rpc_port, 10005, "rpc 端口");
DEFINE_int32(rpc_timeout, 30, "rpc 空闲超时秒");
DEFINE_int32(rpc_threads, 4, "rpc 线程数");
DEFINE_string(access_host, "127.0.0.1:10005", "本服务对外可达 host:port");

DEFINE_string(reg_host, "http://127.0.0.1:2379", "etcd 地址");
DEFINE_string(base_service_dir, "/service", "etcd base 路径");
DEFINE_string(message_service, "/service/message_service", "本服务发现路径");
DEFINE_string(identity_service, "/service/identity_service", "Identity 服务发现路径");
DEFINE_string(media_service, "/service/media_service", "Media 服务发现路径");

DEFINE_string(mysql_host, "127.0.0.1", "mysql host");
DEFINE_int32(mysql_port, 3306, "mysql port");
DEFINE_string(mysql_user, "root", "mysql user");
DEFINE_string(mysql_pwd, "", "mysql pwd");
DEFINE_string(mysql_db, "chatnow_im", "mysql db");
DEFINE_int32(mysql_pool, 32, "mysql pool size");

DEFINE_string(redis_host, "127.0.0.1", "redis host");
DEFINE_int32(redis_port, 6379, "redis port");
DEFINE_int32(redis_db, 0, "redis db");
DEFINE_bool(redis_keepalive, true, "redis keepalive");

DEFINE_string(es_hosts, "http://127.0.0.1:9200", "es 主机列表（逗号分隔）");

DEFINE_string(mq_host, "amqp://guest:guest@127.0.0.1:5672/", "rabbitmq URI");
DEFINE_int32(mq_chat_port, 5672, "rabbitmq 端口（占位）");

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::Logger::create("message", FLAGS_run_mode == 0 ? "debug" : "release");

    chatnow::message::MessageServerBuilder builder;
    builder.make_mysql_object(FLAGS_mysql_host, FLAGS_mysql_port, FLAGS_mysql_user,
                              FLAGS_mysql_pwd, FLAGS_mysql_db, FLAGS_mysql_pool);
    builder.make_redis_object(FLAGS_redis_host, FLAGS_redis_port, FLAGS_redis_db,
                              FLAGS_redis_keepalive);
    std::vector<std::string> es_hosts;
    {
        std::string s = FLAGS_es_hosts;
        size_t pos;
        while((pos = s.find(',')) != std::string::npos) {
            es_hosts.push_back(s.substr(0, pos));
            s = s.substr(pos + 1);
        }
        if(!s.empty()) es_hosts.push_back(s);
    }
    builder.make_es_object(es_hosts);
    // make_mq_object 签名按现有 message_server.cc 原样保留：
    //   builder.make_mq_object(FLAGS_mq_user, FLAGS_mq_pwd, FLAGS_mq_host,
    //                          FLAGS_chat_msg_exchange, FLAGS_msg_queue_db,
    //                          FLAGS_es_index_exchange, FLAGS_msg_queue_es,
    //                          FLAGS_push_queue, ...);
    // 复制旧 message/source/message_server.cc 中现有调用即可（参数列表沿用）
    builder.make_mq_object(/* 按旧 message_server.cc 实参列表逐字复制 */);
    builder.make_discovery_object(FLAGS_reg_host, FLAGS_base_service_dir,
                                  FLAGS_identity_service, FLAGS_media_service);
    builder.make_registry_object(FLAGS_reg_host, FLAGS_message_service, FLAGS_access_host);
    builder.make_rpc_object(FLAGS_rpc_port, FLAGS_rpc_timeout, FLAGS_rpc_threads);
    auto server = builder.build();
    server->start();
    return 0;
}
```

> **说明**：实际 main 中 make_mq_object 的签名按现有实现（旧 message_server.cc）原样保留，gflag 名也按现有 conf 一致。本步只要服务能链 + 启动。

- [ ] **Step 4: 修改 message/CMakeLists.txt**

```cmake
# message/CMakeLists.txt（替换 proto_files 列表）
set(proto_files
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/common/envelope.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/common/types.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/common/error.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/identity/identity_service.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/media/media_service.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/message/message_types.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/message/message_internal.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/message/message_service.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/push/notify.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/../proto/push/push_service.proto"
)
# 其余沿用旧文件结构
```

- [ ] **Step 5: 修改 conf/message_server.conf**

```
--message_service=/service/message_service
--identity_service=/service/identity_service
--media_service=/service/media_service
# 其余 mysql/redis/es/mq/etcd 配置保持
```

- [ ] **Step 6: Commit**

```bash
git add message/source/message_server.h message/source/message_server.cc message/CMakeLists.txt conf/message_server.conf
git commit -m "message: 服务骨架重写（类切换 + Builder + 15 RPC placeholder）"
```

---

## Task 11: 实现 4 个读取类 RPC（GetHistory / SyncMessages / GetMessagesById / SearchMessages）

**Files:**
- Modify: `message/source/message_server.h`

实现要点（每个 RPC 替换 T10 的 `throw ServiceError(kSystemInternalError, "... not implemented")` placeholder）：

### GetHistory

```cpp
void GetHistory(::google::protobuf::RpcController* base_cntl,
                const GetHistoryReq* req, GetHistoryRsp* rsp,
                ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->limit() <= 0 || req->limit() > kMaxLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "limit out of range");
        if (req->before_seq() == 0)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "before_seq must > 0");
        require_member_(req->conversation_id(), auth.user_id);

        auto db_msgs = _mysql_msg->select_history(req->conversation_id(),
                                                  req->before_seq(),
                                                  req->limit() + 1);  // +1 探 has_more
        bool has_more = (static_cast<int>(db_msgs.size()) > req->limit());
        if (has_more) db_msgs.pop_back();

        for (auto &m : db_msgs) {
            auto *out = rsp->add_messages();
            convert_db_message_to_proto_(m, out);
        }
        std::vector<unsigned long> mids;
        for (auto &m : rsp->messages()) mids.push_back(m.message_id());
        fill_reactions_for_messages_(mids, auth.user_id, rsp->mutable_messages());
        fill_pin_flag_for_messages_(req->conversation_id(), mids, rsp->mutable_messages());

        rsp->set_has_more(has_more);
    });
}
```

### SyncMessages

```cpp
void SyncMessages(::google::protobuf::RpcController* base_cntl,
                  const SyncMessagesReq* req, SyncMessagesRsp* rsp,
                  ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->limit() <= 0 || req->limit() > kMaxLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "limit out of range");
        require_member_(req->conversation_id(), auth.user_id);

        auto db_msgs = _mysql_msg->select_after(req->conversation_id(),
                                                 req->after_seq(),
                                                 req->limit() + 1);
        bool has_more = (static_cast<int>(db_msgs.size()) > req->limit());
        if (has_more) db_msgs.pop_back();

        for (auto &m : db_msgs) {
            auto *out = rsp->add_messages();
            convert_db_message_to_proto_(m, out);
        }
        std::vector<unsigned long> mids;
        for (auto &m : rsp->messages()) mids.push_back(m.message_id());
        fill_reactions_for_messages_(mids, auth.user_id, rsp->mutable_messages());
        fill_pin_flag_for_messages_(req->conversation_id(), mids, rsp->mutable_messages());

        rsp->set_has_more(has_more);
        rsp->set_latest_seq(_mysql_msg->select_max_seq_by_conversation(req->conversation_id()));
    });
}
```

### GetMessagesById

```cpp
void GetMessagesById(::google::protobuf::RpcController* base_cntl,
                     const GetMessagesByIdReq* req, GetMessagesByIdRsp* rsp,
                     ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->message_ids_size() == 0 || req->message_ids_size() > kMaxLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "message_ids size out of range");
        std::vector<unsigned long> mids;
        for (int i = 0; i < req->message_ids_size(); ++i)
            mids.push_back(static_cast<unsigned long>(req->message_ids(i)));
        auto db_msgs = _mysql_msg->select_by_ids(mids);

        // 权限：只返回 caller 是成员的会话的消息
        for (auto &m : db_msgs) {
            auto self = _mysql_member->select_self(m.session_id(), auth.user_id);
            if (!self || self->is_quit()) continue;
            auto *out = rsp->add_messages();
            convert_db_message_to_proto_(m, out);
        }
        std::vector<unsigned long> out_mids;
        for (auto &m : rsp->messages()) out_mids.push_back(m.message_id());
        // GetMessagesById 跨会话，pin/reaction 按 mid 自然分组聚合
        fill_reactions_by_mids_(out_mids, auth.user_id, rsp->mutable_messages());
        // is_pinned 按每条 message 自己的 conversation_id 分组查（YAGNI v1：逐条 query 或留空）
    });
}
```

### SearchMessages

```cpp
void SearchMessages(::google::protobuf::RpcController* base_cntl,
                    const SearchMessagesReq* req, SearchMessagesRsp* rsp,
                    ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->keyword().empty())
            throw ::chatnow::ServiceError(::chatnow::error::kMessageContentInvalid,
                                          "keyword empty");
        if (req->limit() <= 0 || req->limit() > kMaxSearchLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "limit out of range");
        require_member_(req->conversation_id(), auth.user_id);

        // 走现有 ESMessage::search（不支持 cursor，本期 v1 先返回全量；
        //   cursor/has_more 留空，后续 ES P95 优化时再加 search_after）
        auto es_results = _es_msg->search(req->keyword(), req->conversation_id(),
                                          req->limit());
        for (auto &m : es_results) {
            auto *out = rsp->add_messages();
            convert_db_message_to_proto_(m, out);
        }
        rsp->set_has_more(false);
        rsp->set_next_cursor("");
    });
}
```

**辅助方法**（在 private 段加）：

```cpp
private:
    /* require_member: caller 必须是会话成员，否则抛 kConversationNotMember */
    void require_member_(const std::string &cid, const std::string &uid) {
        if (uid == kSystemUserId) return;  // 内部调用放行
        auto self = _mysql_member->select_self(cid, uid);
        if (!self || self->is_quit())
            throw ::chatnow::ServiceError(::chatnow::error::kConversationNotMember,
                                          "not member of conversation");
    }

    /* _conv_role: 取 caller 在会话内角色；不存在抛 kConversationNotMember */
    MemberRole _conv_role_(const std::string &cid, const std::string &uid) {
        if (uid == kSystemUserId) return MemberRole::OWNER;
        auto self = _mysql_member->select_self(cid, uid);
        if (!self || self->is_quit())
            throw ::chatnow::ServiceError(::chatnow::error::kConversationNotMember,
                                          "not member of conversation");
        return self->role();
    }

    /* now_ms_: 毫秒时间戳 */
    int64_t now_ms_() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /* convert_db_message_to_proto_: ODB Message → chatnow.message.Message proto */
    void convert_db_message_to_proto_(const Message &m, chatnow::message::Message *out) {
        out->set_message_id(m.message_id());
        out->set_seq_id(m.seq_id());
        out->set_conversation_id(m.session_id());
        out->set_sender_id(m.user_id());
        out->set_created_at_ms(boost::posix_time::to_iso_extended_string(m.create_time()).empty()
                               ? 0 : (m.create_time() - boost::posix_time::ptime(boost::gregorian::date(1970,1,1))).total_milliseconds());
        if (!m.client_msg_id().empty()) out->set_client_msg_id(m.client_msg_id());
        out->set_status(static_cast<chatnow::message::MessageStatus>(static_cast<int>(m.status())));
        // content 反序列化：ODB content 是 text，proto MessageContent 是 oneof
        // 简化方案：根据 message_type 走对应分支；详细在 T11 实施时按现有 onDBMessage 反推
        // 本步用 TextContent 占位，覆盖大多数情况
        if (m.message_type() == MessageType::TEXT) {
            out->mutable_content()->set_type(chatnow::message::TEXT);
            out->mutable_content()->mutable_text()->set_text(m.content());
        }
        // TODO: 其它类型按 ImageContent/FileContent/AudioContent/VideoContent 等填
    }

    /* fill_reactions_for_messages_ / fill_pin_flag_for_messages_: 在 T12-T13 完整实现 */
    void fill_reactions_for_messages_(const std::vector<unsigned long> &mids,
                                      const std::string &caller_uid,
                                      ::google::protobuf::RepeatedPtrField<chatnow::message::Message>* msgs) {
        if (mids.empty()) return;
        auto rows = _mysql_reaction->select_by_messages(mids);
        // 按 (mid, emoji) 分组聚合
        std::map<unsigned long, std::map<std::string, std::vector<std::string>>> grouped;
        for (auto &r : rows) grouped[r.message_id][r.emoji].push_back(r.user_id);
        for (auto &m : *msgs) {
            auto it = grouped.find(m.message_id());
            if (it == grouped.end()) continue;
            for (auto &[emoji, uids] : it->second) {
                auto *g = m.add_reactions();
                g->set_emoji(emoji);
                g->set_count(static_cast<int>(uids.size()));
                bool self = false;
                for (size_t i = 0; i < uids.size(); ++i) {
                    if (uids[i] == caller_uid) self = true;
                    if (i < 3) g->add_recent_user_ids(uids[i]);
                }
                g->set_self_reacted(self);
            }
        }
    }
    void fill_pin_flag_for_messages_(const std::string &cid,
                                     const std::vector<unsigned long> &mids,
                                     ::google::protobuf::RepeatedPtrField<chatnow::message::Message>* msgs) {
        if (mids.empty()) return;
        auto pinned = _mysql_pin->list_pinned_in(cid, mids);
        std::set<unsigned long> pset(pinned.begin(), pinned.end());
        for (auto &m : *msgs) m.set_is_pinned(pset.count(m.message_id()) > 0);
    }
    void fill_reactions_by_mids_(const std::vector<unsigned long> &mids,
                                 const std::string &caller_uid,
                                 ::google::protobuf::RepeatedPtrField<chatnow::message::Message>* msgs) {
        // 与 fill_reactions_for_messages_ 一致逻辑，仅给 GetMessagesById 跨会话使用
        fill_reactions_for_messages_(mids, caller_uid, msgs);
    }
```

> **说明**：`convert_db_message_to_proto_` 的 content 反序列化只展示了 TEXT 分支占位；其它 IMAGE/FILE/AUDIO/VIDEO/LOCATION/STICKER/SYSTEM_NOTICE 分支的填充逻辑参考现有 `onDBMessage` 反向推导（旧 message_server.h:583-620 区段保存了 proto MessageContent → DB content_text 的序列化），本任务实施时需要把那段反向写一份反序列化。

- [ ] **Step 1: 替换 4 个 RPC 实现**

按上述代码替换 T10 占位。

- [ ] **Step 2: 验证字段名**

Run: `grep -nE "select_history|select_after|select_max_seq_by_conversation" message/source/message_server.h`
Expected: 4+ 处使用

- [ ] **Step 3: Commit**

```bash
git add message/source/message_server.h
git commit -m "message: 实现 4 个读取类 RPC（GetHistory/SyncMessages/GetMessagesById/SearchMessages）"
```

---

## Task 12: 实现 RecallMessage RPC + publish_recalled_notify_

**Files:**
- Modify: `message/source/message_server.h`

替换 T10 的 RecallMessage placeholder 为：

```cpp
void RecallMessage(::google::protobuf::RpcController* base_cntl,
                   const RecallMessageReq* req, RecallMessageRsp* rsp,
                   ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
        if (!msg)
            throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid not found");
        if (msg->session_id() != req->conversation_id())
            throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "cid mismatch");
        if (msg->status() == MessageStatus::REVOKED)
            throw ::chatnow::ServiceError(::chatnow::error::kMessageAlreadyRecalled,
                                          "already recalled");
        if (msg->status() == MessageStatus::DELETED)
            throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "deleted");

        auto role = _conv_role_(req->conversation_id(), auth.user_id);
        bool is_admin = (role == MemberRole::OWNER || role == MemberRole::ADMIN);
        bool is_self  = (msg->user_id() == auth.user_id);
        boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
        boost::posix_time::ptime epoch(boost::gregorian::date(1970,1,1));
        int64_t created_ms = (msg->create_time() - epoch).total_milliseconds();
        int64_t age_ms = now_ms_() - created_ms;
        if (!is_admin) {
            if (!is_self)
                throw ::chatnow::ServiceError(::chatnow::error::kConversationNoPermission,
                                              "not msg author");
            if (age_ms >= kRecallTimeoutMs)
                throw ::chatnow::ServiceError(::chatnow::error::kMessageRecallTimeout,
                                              "exceed 120s window");
        }

        if (!_mysql_msg->update_status_to_recalled(static_cast<unsigned long>(req->message_id())))
            throw ::chatnow::ServiceError(::chatnow::error::kMessageAlreadyRecalled,
                                          "race lost or not recallable");

        publish_recalled_notify_(req->conversation_id(),
                                 static_cast<unsigned long>(req->message_id()));
    });
}
```

辅助方法：

```cpp
private:
    /* publish_recalled_notify_: fail-soft；MQ 失败仅 ERROR + 入 push_outbox */
    void publish_recalled_notify_(const std::string &cid, unsigned long mid) {
        if (!_push_publisher) {
            LOG_WARN("publish_recalled_notify: no push_publisher; skip");
            return;
        }
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::MESSAGE_RECALLED_NOTIFY);
        if (auto trace = ::chatnow::log::LogContext::get_trace_id(); !trace.empty())
            nm.set_trace_id(trace);
        auto *r = nm.mutable_message_recalled();
        r->set_conversation_id(cid);
        r->set_message_id(static_cast<int64_t>(mid));
        std::string payload = nm.SerializeAsString();
        try {
            _push_publisher->publish_confirm(payload, /*headers=*/{},
                [cid, mid, payload, this](const std::string& err) {
                    LOG_WARN("publish_recalled_notify failed cid={} mid={}: {}; enqueue push_outbox",
                             cid, mid, err);
                    if (_push_outbox) _push_outbox->enqueue(payload, std::time(nullptr));
                });
        } catch (std::exception &e) {
            LOG_ERROR("publish_recalled_notify exception cid={} mid={}: {}; enqueue push_outbox",
                      cid, mid, e.what());
            if (_push_outbox) _push_outbox->enqueue(payload, std::time(nullptr));
        }
    }
```

- [ ] **Step 1: 替换 RecallMessage 实现 + 加 publish_recalled_notify_**

- [ ] **Step 2: 验证**

Run: `grep -nE "publish_recalled_notify_|MESSAGE_RECALLED_NOTIFY|kMessageRecallTimeout" message/source/message_server.h`
Expected: 多处命中

- [ ] **Step 3: Commit**

```bash
git add message/source/message_server.h
git commit -m "message: 实现 RecallMessage（本人 2min OR OWNER/ADMIN）+ recalled notify fail-soft"
```

---

## Task 13: 实现 3 个 Reaction RPC（AddReaction / RemoveReaction / GetReactions）+ publish_reaction_notify_

**Files:**
- Modify: `message/source/message_server.h`

```cpp
void AddReaction(::google::protobuf::RpcController* base_cntl,
                 const AddReactionReq* req, AddReactionRsp* rsp,
                 ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->emoji().empty() || req->emoji().size() > kMaxEmojiBytes)
            throw ::chatnow::ServiceError(::chatnow::error::kMessageContentInvalid,
                                          "emoji length invalid");
        auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
        if (!msg) throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid");
        require_member_(msg->session_id(), auth.user_id);
        if (!_mysql_reaction->insert(static_cast<unsigned long>(req->message_id()),
                                     auth.user_id, req->emoji()))
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError,
                                          "reaction insert failed");

        publish_reaction_notify_(msg->user_id(), msg->session_id(),
                                 static_cast<unsigned long>(req->message_id()),
                                 auth.user_id, req->emoji(), /*added=*/true);
    });
}

void RemoveReaction(::google::protobuf::RpcController* base_cntl,
                    const RemoveReactionReq* req, RemoveReactionRsp* rsp,
                    ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->emoji().empty() || req->emoji().size() > kMaxEmojiBytes)
            throw ::chatnow::ServiceError(::chatnow::error::kMessageContentInvalid, "emoji");
        auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
        if (!msg) throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid");
        require_member_(msg->session_id(), auth.user_id);
        if (!_mysql_reaction->remove(static_cast<unsigned long>(req->message_id()),
                                     auth.user_id, req->emoji()))
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError, "remove failed");
        // 不推送通知（避免扣赞骚扰）
    });
}

void GetReactions(::google::protobuf::RpcController* base_cntl,
                  const GetReactionsReq* req, GetReactionsRsp* rsp,
                  ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
        if (!msg) throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid");
        require_member_(msg->session_id(), auth.user_id);

        auto rows = _mysql_reaction->select_by_message(static_cast<unsigned long>(req->message_id()));
        // GROUP BY emoji
        std::map<std::string, std::vector<std::string>> grouped;
        for (auto &r : rows) grouped[r.emoji].push_back(r.user_id);
        for (auto &[emoji, uids] : grouped) {
            auto *g = rsp->add_reactions();
            g->set_emoji(emoji);
            g->set_count(static_cast<int>(uids.size()));
            bool self = false;
            for (size_t i = 0; i < uids.size(); ++i) {
                if (uids[i] == auth.user_id) self = true;
                if (i < 3) g->add_recent_user_ids(uids[i]);
            }
            g->set_self_reacted(self);
        }
    });
}
```

辅助方法：

```cpp
private:
    /* publish_reaction_notify_: 仅推消息 sender_id；fail-soft */
    void publish_reaction_notify_(const std::string &target_uid,
                                  const std::string &cid,
                                  unsigned long mid,
                                  const std::string &actor_uid,
                                  const std::string &emoji,
                                  bool added) {
        if (!_push_publisher || target_uid == actor_uid) return;  // 自己点不通知自己
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::REACTION_CHANGED_NOTIFY);
        if (auto trace = ::chatnow::log::LogContext::get_trace_id(); !trace.empty())
            nm.set_trace_id(trace);
        auto *r = nm.mutable_reaction_changed();
        r->set_conversation_id(cid);
        r->set_message_id(static_cast<int64_t>(mid));
        r->set_actor_user_id(actor_uid);
        r->set_emoji(emoji);
        r->set_added(added);
        // PushToUserReq.user_id 路由 → 由 Push 服务端按 single user 投递（target_uid 仅一个）
        // 本服务投 push_queue 的 payload 是 NotifyMessage；Push 服务消费时按 target_user_ids 决定路由
        // YAGNI v1：仅放进 NotifyMessage trace；目标 uid 由 PushToUser RPC 调用方提供
        // 由于现有 onDBMessage 链路是把 NotifyMessage 投 MQ 后由 Push 自己路由会话所有成员，
        // Reaction 单点推送需要绕过 push_queue，直接调 PushService.PushToUser RPC。
        //
        // 简化方案（v1）：fire-and-forget，调 PushService.PushToUser
        try {
            auto channel = _mm_channels ? _mm_channels->choose("push_service") : nullptr;
            if (!channel) { LOG_WARN("push channel unavailable; skip reaction notify"); return; }
            chatnow::push::PushService_Stub stub(channel.get());
            chatnow::push::PushToUserReq preq;
            preq.set_request_id("reaction-notify");
            preq.set_user_id(target_uid);
            *preq.mutable_notify() = nm;
            chatnow::push::PushToUserRsp prsp;
            brpc::Controller pcntl;
            pcntl.set_timeout_ms(500);
            stub.PushToUser(&pcntl, &preq, &prsp, brpc::DoNothing());  // fire-and-forget
        } catch (std::exception &e) {
            LOG_ERROR("publish_reaction_notify exception target={} mid={}: {}", target_uid, mid, e.what());
        }
    }
```

> **注意**：`_mm_channels` 中 push_service 的 channel 需要在 `make_discovery_object` 里 declared("push_service")。本期没把 push 加入下游，但 spec 要求强类型 publish 时 PushToUser，所以 T10 的 builder 需补一行：`_mm_channels->declared("push_service");` —— 在 T10 实施时一并加（或 T13 实施时回头加）。

- [ ] **Step 1: 替换 3 个 Reaction RPC + 加 publish_reaction_notify_**

- [ ] **Step 2: 修 T10 builder，把 push_service 加入 declared 列表**

修改 `make_discovery_object`：

```cpp
_mm_channels->declared(_identity_service_name);
_mm_channels->declared(_media_service_name);
_mm_channels->declared("/service/push_service");  // 新增
```

- [ ] **Step 3: 验证**

Run: `grep -nE "publish_reaction_notify_|REACTION_CHANGED_NOTIFY|MessageReactionTable" message/source/message_server.h`
Expected: 多处命中

- [ ] **Step 4: Commit**

```bash
git add message/source/message_server.h
git commit -m "message: 实现 Reaction 3 RPC（Add/Remove/Get）+ 仅推 sender 的强类型 notify"
```

---

## Task 14: 实现 3 个 Pin RPC（PinMessage / UnpinMessage / ListPinnedMessages）+ publish_pin_notify_

**Files:**
- Modify: `message/source/message_server.h`

```cpp
void PinMessage(::google::protobuf::RpcController* base_cntl,
                const PinMessageReq* req, PinMessageRsp* rsp,
                ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        auto role = _conv_role_(req->conversation_id(), auth.user_id);
        if (role != MemberRole::OWNER && role != MemberRole::ADMIN)
            throw ::chatnow::ServiceError(::chatnow::error::kConversationNoPermission,
                                          "only OWNER/ADMIN can pin");
        auto msg = _mysql_msg->select_by_id(static_cast<unsigned long>(req->message_id()));
        if (!msg || msg->session_id() != req->conversation_id())
            throw ::chatnow::ServiceError(::chatnow::error::kMessageNotFound, "mid");
        if (_mysql_pin->count_by_conversation(req->conversation_id()) >= kPinLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "pin limit exceeded (10)");

        if (!_mysql_pin->insert(req->conversation_id(),
                                static_cast<unsigned long>(req->message_id()),
                                auth.user_id))
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError, "pin insert failed");

        publish_pin_notify_(req->conversation_id(),
                            static_cast<unsigned long>(req->message_id()),
                            auth.user_id, /*pinned=*/true);
    });
}

void UnpinMessage(::google::protobuf::RpcController* base_cntl,
                  const UnpinMessageReq* req, UnpinMessageRsp* rsp,
                  ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        auto role = _conv_role_(req->conversation_id(), auth.user_id);
        if (role != MemberRole::OWNER && role != MemberRole::ADMIN)
            throw ::chatnow::ServiceError(::chatnow::error::kConversationNoPermission,
                                          "only OWNER/ADMIN can unpin");
        if (!_mysql_pin->remove(req->conversation_id(),
                                static_cast<unsigned long>(req->message_id())))
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInternalError, "unpin failed");

        publish_pin_notify_(req->conversation_id(),
                            static_cast<unsigned long>(req->message_id()),
                            auth.user_id, /*pinned=*/false);
    });
}

void ListPinnedMessages(::google::protobuf::RpcController* base_cntl,
                        const ListPinnedReq* req, ListPinnedRsp* rsp,
                        ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        require_member_(req->conversation_id(), auth.user_id);
        auto mids = _mysql_pin->list_by_conversation(req->conversation_id(), kPinLimit);
        if (mids.empty()) return;
        auto db_msgs = _mysql_msg->select_by_ids(mids);
        for (auto &m : db_msgs) {
            if (m.status() == MessageStatus::DELETED) continue;
            auto *out = rsp->add_messages();
            convert_db_message_to_proto_(m, out);
        }
        std::vector<unsigned long> out_mids;
        for (auto &m : rsp->messages()) out_mids.push_back(m.message_id());
        fill_reactions_for_messages_(out_mids, auth.user_id, rsp->mutable_messages());
        // 已知 pinned，全设 true
        for (auto &m : *rsp->mutable_messages()) m.set_is_pinned(true);
    });
}
```

辅助方法：

```cpp
private:
    /* publish_pin_notify_: 推会话所有成员；fail-soft */
    void publish_pin_notify_(const std::string &cid, unsigned long mid,
                             const std::string &actor_uid, bool pinned) {
        chatnow::push::NotifyMessage nm;
        nm.set_notify_type(chatnow::push::PIN_CHANGED_NOTIFY);
        if (auto trace = ::chatnow::log::LogContext::get_trace_id(); !trace.empty())
            nm.set_trace_id(trace);
        auto *p = nm.mutable_pin_changed();
        p->set_conversation_id(cid);
        p->set_message_id(static_cast<int64_t>(mid));
        p->set_actor_user_id(actor_uid);
        p->set_is_pinned(pinned);

        // 按现有 onDBMessage 模式投 push_queue（Push 服务从 NotifyMessage 取所有 cid 成员路由）
        // 简化方案：复用 push_queue Publisher
        if (!_push_publisher) { LOG_WARN("publish_pin_notify: no push_publisher; skip"); return; }
        std::string payload = nm.SerializeAsString();
        try {
            _push_publisher->publish_confirm(payload, /*headers=*/{},
                [cid, mid, payload, this](const std::string& err) {
                    LOG_WARN("publish_pin_notify failed cid={} mid={}: {}; enqueue push_outbox",
                             cid, mid, err);
                    if (_push_outbox) _push_outbox->enqueue(payload, std::time(nullptr));
                });
        } catch (std::exception &e) {
            LOG_ERROR("publish_pin_notify exception cid={} mid={}: {}; enqueue push_outbox",
                      cid, mid, e.what());
            if (_push_outbox) _push_outbox->enqueue(payload, std::time(nullptr));
        }
    }
```

- [ ] **Step 1: 替换 3 个 Pin RPC + 加 publish_pin_notify_**

- [ ] **Step 2: 验证**

Run: `grep -nE "publish_pin_notify_|kPinLimit|MessagePinTable" message/source/message_server.h`
Expected: 多处命中

- [ ] **Step 3: Commit**

```bash
git add message/source/message_server.h
git commit -m "message: 实现 Pin 3 RPC（Pin/Unpin/ListPinned）+ 强类型 notify fail-soft"
```

---

## Task 15: 实现剩余 5 个 RPC（DeleteMessages / ClearConversation / SelectByClientMsgId / UpdateReadAck）

**Files:**
- Modify: `message/source/message_server.h`

```cpp
void DeleteMessages(::google::protobuf::RpcController* base_cntl,
                    const DeleteMessagesReq* req, DeleteMessagesRsp* rsp,
                    ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        if (req->message_ids_size() == 0 || req->message_ids_size() > kMaxLimit)
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "message_ids size out of range");
        require_member_(req->conversation_id(), auth.user_id);
        std::vector<unsigned long> mids;
        for (int i = 0; i < req->message_ids_size(); ++i)
            mids.push_back(static_cast<unsigned long>(req->message_ids(i)));
        (void)_mysql_user_timeline->delete_by_message_ids(auth.user_id,
                                                          req->conversation_id(),
                                                          mids);
    });
}

void ClearConversation(::google::protobuf::RpcController* base_cntl,
                       const ClearConversationReq* req, ClearConversationRsp* rsp,
                       ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        require_member_(req->conversation_id(), auth.user_id);
        (void)_mysql_user_timeline->delete_by_conversation(auth.user_id,
                                                            req->conversation_id());
    });
}

void SelectByClientMsgId(::google::protobuf::RpcController* base_cntl,
                         const SelectByClientMsgIdReq* req, SelectByClientMsgIdRsp* rsp,
                         ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        // 内部接口：放行 __system__；外部 caller 必须用自己 user_id 查
        std::string lookup_uid = (auth.user_id == kSystemUserId)
            ? "" /* 使用 client_msg_id 全局唯一性，但 DAO 需要 uid */
            : auth.user_id;
        if (lookup_uid.empty())
            throw ::chatnow::ServiceError(::chatnow::error::kSystemInvalidArgument,
                                          "system caller must specify uid via metadata");
        auto msg = _mysql_msg->select_by_client_msg(lookup_uid, req->client_msg_id());
        if (msg) convert_db_message_to_proto_(*msg, rsp->mutable_message());
    });
}

void UpdateReadAck(::google::protobuf::RpcController* base_cntl,
                   const UpdateReadAckReq* req, UpdateReadAckRsp* rsp,
                   ::google::protobuf::Closure* done) override {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(base_cntl);
    HANDLE_RPC(cntl, req, rsp, {
        // 走 ConversationMemberTable 已有的 _atomic_advance_seq("last_ack_seq", ...)
        // 假设 DAO 暴露 update_last_ack_seq 方法（与 conversation 服务的 update_last_read_seq 同模式）
        if (!_mysql_member->update_last_ack_seq(req->conversation_id(),
                                                auth.user_id,
                                                req->seq_id())) {
            // 不存在 / 已大于该 seq → 视幂等成功，不抛
        }
    });
}
```

> **注意**：`update_last_ack_seq` 需要存在于 `ConversationMemberTable`。如果不存在，T15 实施时需要在 `common/dao/mysql_conversation_member.hpp` 加一个方法（与现有 `update_last_read_seq` 同模式，仅字段名换 `last_ack_seq`）。先 grep 验证存在性。

- [ ] **Step 1: grep 验证 update_last_ack_seq 存在**

Run: `grep -n "update_last_ack_seq" common/dao/mysql_conversation_member.hpp`
Expected: 至少 1 行（如果不存在，T15 包含加方法的 sub-step）

- [ ] **Step 2: 如果不存在，在 ConversationMemberTable 里加 update_last_ack_seq**

模仿 `update_last_read_seq` 实现：

```cpp
    bool update_last_ack_seq(const std::string &cid, const std::string &uid,
                             unsigned long new_seq) {
        return _atomic_advance_seq("last_ack_seq", cid, uid, new_seq);
    }
```

- [ ] **Step 3: 替换 4 个剩余 RPC**

- [ ] **Step 4: 验证**

Run: `grep -nE "DeleteMessages|ClearConversation|SelectByClientMsgId|UpdateReadAck" message/source/message_server.h | grep -v throw`
Expected: 真实实现命中

- [ ] **Step 5: Commit**

```bash
git add message/source/message_server.h common/dao/mysql_conversation_member.hpp
git commit -m "message: 实现 DeleteMessages/ClearConversation/SelectByClientMsgId/UpdateReadAck"
```

---

## Task 16: Gateway HTTP handler 切换（5 旧 + 删 2 + 新增 11）

**Files:**
- Modify: `gateway/source/gateway_server.h`

变更点：

1. 删 GetUnreadCount handler（前端走 Conversation.SelfMemberInfo.unread_count）
2. 删 GetOfflineMsg handler（前端走 SyncMessages with after_seq=0）
3. 切换 GetHistory / GetRecent (→SyncMessages) / Search 的 stub 与 RPC 名
4. 新增 11 个 handler：Recall / Reactions×3 / Pin×3 / Delete×2 / Clear / GetMessagesById

每个新 handler 范式：

```cpp
void RecallMessage(const httplib::Request &request, httplib::Response &response) {
    auto _auth = check_auth(request);  // 现有鉴权 helper
    if (!_auth.ok) { make_error_response(response, kAuthTokenInvalid); return; }
    chatnow::message::RecallMessageReq req;
    chatnow::message::RecallMessageRsp rsp;
    req.ParseFromString(request.body);
    auto channel = _mm_channels->choose(_message_service_name);
    if (!channel) { make_error_response(response, kSystemUnavailable); return; }
    brpc::Controller cntl;
    apply_auth_to_brpc(request, cntl, _auth);
    chatnow::message::MessageService_Stub stub(channel.get());
    stub.RecallMessage(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        rsp.mutable_header()->set_success(false);
        rsp.mutable_header()->set_error_code(kSystemUnavailable);
        rsp.mutable_header()->set_error_message(cntl.ErrorText());
    }
    response.set_content(rsp.SerializeAsString(), "application/x-protobuf");
}
```

11 个新 handler 全部按这个范式（仅 RPC 名 + Req/Rsp 类型不同）：
- RecallMessage → `/service/message/recall`
- AddReaction → `/service/message/reaction/add`
- RemoveReaction → `/service/message/reaction/remove`
- GetReactions → `/service/message/reaction/list`
- PinMessage → `/service/message/pin`
- UnpinMessage → `/service/message/unpin`
- ListPinnedMessages → `/service/message/list_pinned`
- DeleteMessages → `/service/message/delete`
- ClearConversation → `/service/message/clear`
- GetMessagesById → `/service/message/get_by_id`

- [ ] **Step 1: 删 GetUnreadCount handler + 路由注册**

Run: `grep -n "GetUnreadCount\|GET_UNREAD" gateway/source/gateway_server.h`
找到所有引用并删除（function 定义 + Post 路由注册 + 任何 helper）

- [ ] **Step 2: 删 GetOfflineMsg handler + 路由注册**

Run: `grep -n "GetOfflineMsg\|GET_OFFLINE" gateway/source/gateway_server.h`
同上删除

- [ ] **Step 3: 切换 GetHistory / GetRecent / Search 三个旧 handler**

把 `MsgStorageService_Stub` → `chatnow::message::MessageService_Stub`，方法名 `GetHistoryMsg` → `GetHistory`、`GetRecentMsg` → `SyncMessages`、`MsgSearch` → `SearchMessages`，Req/Rsp 类型路径切换。鉴权改用 `apply_auth_to_brpc`。

- [ ] **Step 4: 新增 11 个 handler**

按上述 RecallMessage 范式实现。每个 handler 函数 + Post 路由注册。

- [ ] **Step 5: 验证**

Run: `grep -nE "MsgStorageService_Stub" gateway/source/gateway_server.h`
Expected: 0 行

Run: `grep -nE "MessageService_Stub" gateway/source/gateway_server.h`
Expected: 14 处（5 旧切换 + 11 新增 = 16 减去 2 删 = 14）

- [ ] **Step 6: Commit**

```bash
git add gateway/source/gateway_server.h
git commit -m "gateway: message handler 切到 MessageService（5 切 + 2 删 + 11 新）"
```

---

## Task 17: 删旧 proto + cleanup

**Files:**
- Delete: `proto/message.proto`（如还存在）

- [ ] **Step 1: 全仓 grep 验证旧 stub 零引用**

Run: `grep -rn "chatnow::MsgStorageService\|MsgStorageService_Stub" --include="*.h" --include="*.cc" --include="*.cpp" --include="*.hpp"`
Expected: 0 行命中（gateway 已切；message 已重写；无其它使用方）

Run: `grep -rn "GetHistoryMsg\|GetRecentMsg\|MsgSearch\|GetOfflineMsg\|GetUnreadCount" --include="*.h" --include="*.cc" --include="*.cpp" --include="*.hpp" | grep -v specs | grep -v plans`
Expected: 0 行（除 spec / plan 文档外）

- [ ] **Step 2: 删 proto/message.proto（如还存在）**

```bash
ls proto/message.proto 2>&1
# 如果存在
git rm proto/message.proto
```

- [ ] **Step 3: 顶层 CMakeLists.txt 检查不需要改名**

Run: `grep "add_subdirectory(message)" CMakeLists.txt`
Expected: 1 行（message/ 目录沿用）

- [ ] **Step 4: docker-compose.yml 检查 + 同步配置**

```bash
grep -n "message_server\|MsgStorage\|/service/message" docker-compose.yml
```

如服务名 `message_server` 沿用 + gflag 已是 `--message_service=/service/message_service` 则无需改动；否则按 conf/message_server.conf 同步。

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "cleanup(message): 删除 proto/message.proto + 顶层引用同步"
```

---

## Task 18: onDBMessage / onESIndexMessage 包名切换 + SeqGen 启动回填实现

**Files:**
- Modify: `message/source/message_server.h`（替换 T10 的 placeholder）

本任务把 T10 中的 `onDBMessage` / `onESIndexMessage` placeholder 替换为真实业务逻辑：

1. 复制旧 `message/source/message_server.h:583-749`（onDBMessage）+ `:861-933`（onESIndexMessage）的逻辑
2. 把 `chatnow::InternalMessage` → `chatnow::message::internal::InternalMessage`
3. 把 `chatnow::ESIndexEvent` → `chatnow::message::internal::ESIndexEvent`
4. 把 `chatnow::NotifyMessage` → `chatnow::push::NotifyMessage`
5. 把对应字段访问按新 proto 字段名调整（`Message.message_id` / `seq_id` / `sender_id` / `created_at_ms` 等已与 proto 对齐）
6. publish 改：构造新 NotifyMessage 后 `SerializeAsString()`

同时实现 `MessageServerBuilder::backfill_seq_from_db_()`：

```cpp
private:
    void backfill_seq_from_db_() {
        // session-seq：扫所有 conversation_member 的 conversation_id distinct，逐个 backfill
        // 简化方案：直接走 mysql_msg 的"distinct conversation_id + max(seq_id)"
        // YAGNI：本期不加专门的 select_max_seq_per_conversation；用 select_active_conversations + 循环
        //
        // 实际实现：通过 ConversationMemberTable 取所有 conversation_id 列表，逐个 select_max_seq_by_conversation
        // 用户级：通过 UserTimelineTable.select_max_user_seq_per_user 一次拿到
        int s_count = 0;
        auto cids = _mysql_member ? _mysql_member->all_conversation_ids() : std::vector<std::string>{};
        for (auto &cid : cids) {
            unsigned long mx = _mysql_msg->select_max_seq_by_conversation(cid);
            if (mx > 0) { _seq_gen->backfill_session(cid, mx + 1); ++s_count; }
        }
        auto user_pairs = _mysql_user_timeline->select_max_user_seq_per_user();
        int u_count = 0;
        for (auto &[uid, mx] : user_pairs) {
            if (mx > 0) { _seq_gen->backfill_user(uid, mx + 1); ++u_count; }
        }
        LOG_INFO("seqgen backfill done sessions={}/{} users={}/{}",
                 s_count, cids.size(), u_count, user_pairs.size());
    }
```

> **DAO 补充**：`ConversationMemberTable::all_conversation_ids()` 不存在则需要在 T18 加（参考 `members(cid)` 实现，改为 distinct conversation_id）：

```cpp
    std::vector<std::string> all_conversation_ids() {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ConversationMember>;
            odb::result<ConversationMember> r(_db->query<ConversationMember>(""));
            std::set<std::string> seen;
            for (auto it = r.begin(); it != r.end(); ++it) seen.insert(it->conversation_id());
            for (auto &c : seen) res.push_back(c);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("all_conversation_ids: {}", e.what());
        }
        return res;
    }
```

- [ ] **Step 1: 替换 onDBMessage 真实业务逻辑（包名切换）**

按现有 `message_server.h:583-749` 的代码搬运 + 5 处包名替换。

- [ ] **Step 2: 替换 onESIndexMessage 真实业务逻辑**

按 `:906-933` 搬运 + 包名替换。

- [ ] **Step 3: 实现 backfill_seq_from_db_**

按上述代码替换 T10 的 LOG_INFO 占位。

- [ ] **Step 4: 如 ConversationMemberTable 没有 all_conversation_ids 方法，添加**

- [ ] **Step 5: 验证**

Run: `grep -E "onDBMessage|onESIndexMessage|backfill_seq_from_db" message/source/message_server.h`
Expected: 真实代码（非 placeholder）

Run: `grep -E "chatnow::InternalMessage\b|chatnow::ESIndexEvent\b|chatnow::NotifyMessage\b" message/source/message_server.h`
Expected: 0 行（旧包名应全部切换）

- [ ] **Step 6: Commit**

```bash
git add message/source/message_server.h common/dao/mysql_conversation_member.hpp
git commit -m "message: onDBMessage/onESIndexMessage 包名切换 + SeqGen 启动回填实现"
```

---

## Task 19: 在 spec 末尾追加 §10 实施记录

**Files:**
- Modify: `docs/superpowers/specs/2026-05-16-message-service-migration-design.md` §10

仿 Relationship / Conversation 同模式，把 plan T1–T18 的 commit 序列、横切 hotfix（如有）、未完成项、给下一位 Agent 的接手清单填入 §10。

- [ ] **Step 1: 取本次 commit 序列**

Run: `git log --oneline 6c8ce74..HEAD | head -30`
Expected: T1–T18 + 任何 hotfix commit

- [ ] **Step 2: 编辑 §10**

按 §11 of `2026-05-16-conversation-service-migration-design.md` 模板填：

- §10.1 状态总览表（18 RPC 代码 / DAO / proto / Gateway / docker / 编译验证 / 集成验收 各项 ✅/❌）
- §10.2 commit 序列
- §10.3 横切 hotfix 列表（如有）
- §10.4 关键设计选择固化记录（撤回 OWNER/ADMIN 双权限 / Reaction 仅推 sender / Pin 上限 10 / fail-soft 策略）
- §10.5 已知阻塞（编译留给 Transmite/Push 迁移期）
- §10.6 给下一位 Agent 的接手清单
- §10.7 结构性约束（不动其它服务 proto / 不要编译验证）

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-05-16-message-service-migration-design.md
git commit -m "docs(spec): message 迁移 spec 加 §10 实施记录"
```

---

## Self-Review

完成上述 18 个 Task 后，自查以下：

- [ ] **Spec coverage**：spec §1–§9 每节是否都有对应 Task 落地？
  - §1 范围目标 → T1–T18 全部
  - §2 proto + RPC 矩阵 → T1–T4（proto）+ T11–T15（RPC 实现）
  - §3 服务类与 handler 模式 → T10（骨架）+ T11–T15（业务）
  - §4 数据层 → T6–T9（DAO）
  - §5 服务结构 / Builder / CMake / docker → T10 + T17
  - §6 验收标准 → 每个 Task 的 Step 验证
  - §7 编译阻塞 / 兼容性 → 不需 Task（约束已落到 spec）
  - §8 工作量 → 不需 Task
  - §9 后续工作 → 不需 Task
  - §10 实施记录 → T19

- [ ] **Placeholder scan**：
  - "TBD" / "TODO" 在 plan 中只允许出现在"留给后续 Task 的 placeholder"位置（T10 的 RPC body / T18 的 onDBMessage 实现说明），其它位置不允许
  - 所有 code block 都给了完整代码

- [ ] **Type consistency 检查**：
  - `kRecallTimeoutMs` / `kPinLimit` / `kMaxLimit` 等常量在 T10 定义，T11–T15 引用 — 一致
  - `MemberRole::OWNER` / `ADMIN` / `NORMAL` 在 odb/conversation_member.hxx — 一致
  - `MessageStatus::REVOKED`（不是 RECALLED）在 odb/message.hxx — 已注意
  - `_mysql_member->select_self()` 返回 `std::shared_ptr<ConversationMember>`，含 `is_quit()` / `role()` — 一致
  - `publish_*_notify_` 方法签名各 Task 一致

如发现问题，inline 修复后继续。

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-05-16-message-service-migration.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
