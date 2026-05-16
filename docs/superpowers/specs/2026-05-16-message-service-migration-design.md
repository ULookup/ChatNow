# Message 服务迁移设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-16
> **范围**: 旧 `message/` 服务（`MessageServiceImpl : public chatnow::MsgStorageService`）迁到新 proto `chatnow.message.MessageService`
> **基线**: `proto/message/message_service.proto` + `proto/message/message_internal.proto` + `proto/message/message_types.proto` + `proto/push/notify.proto` + `proto/push/push_service.proto` + `2026-05-14-cross-cutting-architecture-design.md` §2.6 §2.8 §4.7 §4.9 §5.5 + `2026-05-14-service-migration-design.md` §3.4 §3.6 + `2026-05-13-es-dual-write-redesign.md` + `2026-05-13-message-reliability-hardening.md` §1 + `2026-05-15-relationship-service-migration-design.md`（同模式）+ `2026-05-16-conversation-service-migration-design.md`（同模式）
> **前置**: Identity / Media / Relationship / Conversation 已迁移；Transmite + Push 尚未迁移（本期编译阻塞与 Conversation 当前同质）

---

## 1. 范围与目标

把 `message/source/message_server.h` 从老 `MessageServiceImpl : public chatnow::MsgStorageService` 迁到 `MessageServiceImpl : public chatnow::message::MessageService`。同步：

- **proto 全套清理（4 个文件）**：
  - `message_service.proto`：删 `optional string user_id` / `optional string session_id`（2 处）+ 加 `option cc_generic_services = true` + 全限定（`ResponseHeader` → `chatnow.common.ResponseHeader`；`Message` / `MessagePreview` / `ReactionGroup` → `chatnow.message.*`）
  - `message_internal.proto`：保持 `chatnow.message.internal` 包名 + 加 `option cc_generic_services = true` + 全限定
  - `push/notify.proto`：加 3 个新 `NotifyType`（`REACTION_CHANGED_NOTIFY` / `PIN_CHANGED_NOTIFY` / `READ_RECEIPT_NOTIFY`）+ 对应 3 个 `NotifyXxx` message + `NotifyMessage.oneof notify_remarks` 加 3 个分支 + 加 `option cc_generic_services` + 全限定
  - `push/push_service.proto`：`bytes notify_payload` → `chatnow.push.NotifyMessage notify`（强类型，对齐横切 spec §4.7 / gaps G5）+ 加 `option cc_generic_services` + 全限定
- **15 个 RPC 全部落实**（含 10 个新增：`RecallMessage` / `AddReaction` / `RemoveReaction` / `GetReactions` / `PinMessage` / `UnpinMessage` / `ListPinnedMessages` / `DeleteMessages` / `ClearConversation` / `GetMessagesById`）
- **MQ 拓扑保持现状不变**（已是 DB-then-derive ES 模式，由 `2026-05-13-es-dual-write-redesign.md` 落地，不重构）：
  - `onDBMessage` 反序列化包名切换：`chatnow::InternalMessage` → `chatnow::message::internal::InternalMessage`
  - `onESIndexMessage` 反序列化包名切换：`chatnow::ESIndexEvent` → `chatnow::message::internal::ESIndexEvent`
  - 函数签名 / 回调注册 / 队列名 / push_outbox / es_outbox 全部不变
- **Push publish 强类型化**：Message 服务投 `push_queue` 的 payload 由 raw bytes 改为 `chatnow::push::NotifyMessage.SerializeAsString()`（强类型）。Push 服务现有反序列化路径仍用旧包名 `chatnow::NotifyMessage`，wire format 一致（同名同 tag，proto3 二进制兼容），中间态可上线
- **SeqGen 启动回填**（按 `2026-05-13-message-reliability-hardening.md` §1 实现，之前未做）：`make_rpc_object` 内、订阅 MQ 之前回填；`backfill_session` / `backfill_user` 改 Lua 原子
- **响应模式从 `success/errmsg` 改为 `ResponseHeader`**，handler 一律用 `HANDLE_RPC` 宏 + `throw ServiceError(code, msg)`
- **鉴权字段全走 metadata**，proto 业务体 `user_id` / `session_id` 已删（`SelectByClientMsgId.user_id` 与 `UpdateReadAck.user_id` 字段从 metadata 读取，proto 中删除字段）
- **跨服务 stub 切换**：`UserService_Stub` → `chatnow::identity::IdentityService_Stub`（已迁，可编译）；`FileService_Stub` → `chatnow::media::MediaService_Stub.DownloadFile/UploadFile`（已迁，可编译，循环单数）；权限校验直接读 `conversation_member` 表（DAO `ConversationMemberTable.select_self`，与 Conversation 服务共享 schema），不走 ConversationService RPC——避免 Recall / Pin 等高频路径双跳
- **Gateway message handler 切换**：5 个旧 handler（GetHistory/SyncMessages/SearchMessages 改名+切 stub；GetOfflineMsg / GetUnreadCount 删除）+ 11 个新 handler（recall / reaction×3 / pin×3 / delete×2 / clear / get_by_id）

明确**不做**（YAGNI）：

- ❌ MQ 拓扑重构（FANOUT / DIRECT / dual-write 体系已落地，不动）
- ❌ Transmite / Push 服务本身（独立 spec）
- ❌ Device 模型 / JWT did 字段（独立 Push spec）
- ❌ MarkRead 水位缓存 / write-behind flush（独立 spec，Message 迁移之后下一项）
- ❌ Reaction summary 计数表 / Redis HASH 物化（业界 v1 共识：读时聚合 GROUP BY emoji；万级 reaction 才考虑物化）
- ❌ 全局删除 / "Delete for everyone" 语义（DeleteMessages / ClearConversation 仅自己 timeline；撤回走 RecallMessage 独立 RPC）
- ❌ Conversation 解散通知（`CONVERSATION_DISMISSED_NOTIFY`）补 proto（Push 迁移期顺带做）
- ❌ 多实例 Message 服务同会话 seq 严格有序（已有 `2026-05-14-mq-quorum-and-seq-ordering-design.md`，独立实施）
- ❌ Pin / Reaction 应用层 abuse 限流（无业务驱动）
- ❌ `message_attachment` 多附件落库逻辑变更（schema 已有但本期消息只用单附件路径，对齐现状）
- ❌ HTTP 路径前缀重命名（沿用 `/service/message/*`，与 Relationship `/service/friend/*` 同保留策略）

---

## 2. 服务接口与 proto 字段

### 2.1 proto 改动总览

| 文件 | 改动 |
|---|---|
| `proto/message/message_service.proto` | 加 `option cc_generic_services = true`；引用全限定；删 `SelectByClientMsgIdReq.user_id` 与 `UpdateReadAckReq.user_id`（共 2 处） |
| `proto/message/message_internal.proto` | 加 `option cc_generic_services = true`；引用全限定 |
| `proto/message/message_types.proto` | 不动 |
| `proto/push/notify.proto` | 加 3 个 NotifyType + 3 个 NotifyXxx message + NotifyMessage.oneof 加 3 分支；加 `option cc_generic_services`；引用全限定 |
| `proto/push/push_service.proto` | `bytes notify_payload` → `chatnow.push.NotifyMessage notify`；加 `option cc_generic_services`；引用全限定 |

### 2.2 RPC 矩阵（15 个）

#### 2.2.1 读取类（4）

| RPC | 关键 Req | 关键 Rsp | 备注 |
|---|---|---|---|
| `GetHistory` | `request_id, conversation_id, before_seq, limit≤100` | `header, messages[], has_more` | 必须是会话成员；DAO `select_history(cid, before_seq, limit)` |
| `SyncMessages` | `request_id, conversation_id, after_seq, limit≤100` | `header, messages[], has_more, latest_seq` | `after_seq=0` 等同离线全量拉取（合并旧 GetOfflineMsg）；`latest_seq` 取 `select_max_seq_by_conversation(cid)` |
| `GetMessagesById` | `request_id, message_ids[]≤100` | `header, messages[]` | 用于客户端补拉（reply_to / 转发引用 / 撤回前提示）；DAO `select_by_ids`；权限：要求每条 mid 对应的 cid 当前用户是成员，否则该条静默丢弃 |
| `SearchMessages` | `request_id, conversation_id, keyword, cursor, limit≤50` | `header, messages[], has_more, next_cursor` | `ESMessage::search` ：`bool` 包 `term(conversation_id) + match(content_text) + filter(status!=DELETED)`，`sort=[{created_at_ms desc},{message_id desc}]`，`search_after=cursor` 解析 `[ts, mid]` |

#### 2.2.2 写入类——撤回（1）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `RecallMessage` | `request_id, conversation_id, message_id` | 鉴权：`auth.user_id == msg.sender_id` 且 `now - created_at_ms < 120000ms`；OR `caller.role IN (OWNER, ADMIN)` 任意时间。失败码：`MESSAGE_RECALL_TIMEOUT(4002)` / `MESSAGE_ALREADY_RECALLED(4003)` / `MESSAGE_NOT_FOUND(4001)` / `CONVERSATION_NO_PERMISSION(3003)`。成功 → DAO `update_status_to_recalled(mid)` 并清空 content_text + 推 `MESSAGE_RECALLED_NOTIFY` 给会话所有成员（fail-soft：MQ 发失败 ERROR + 不抛 ServiceError） |

撤回时间窗常量：`kRecallTimeoutMs = 120 * 1000`（与 spec §1 对齐微信 / QQ 业界共识）。

#### 2.2.3 写入类——反应（3）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `AddReaction` | `request_id, message_id, emoji` | 必须是该消息会话成员；emoji 长度 ≤16 字节，否则 `MESSAGE_CONTENT_INVALID(4004)`；`(message_id, user_id, emoji)` 唯一索引（已存在视幂等成功）；推 `REACTION_CHANGED_NOTIFY` 仅给消息 sender |
| `RemoveReaction` | `request_id, message_id, emoji` | 删 `(message_id, user_id, emoji)` 行；不存在视幂等成功；不推送（避免扣赞通知骚扰） |
| `GetReactions` | `request_id, message_id` | 必须是会话成员；返回 `repeated ReactionGroup` —— 每 emoji 一个分组，含 `count, recent_user_ids[≤3], self_reacted`；服务层在 handler 内对 `message_reaction` 全行做 GROUP BY emoji 聚合（业界 v1 共识：读时聚合，无 summary 计数表） |

#### 2.2.4 写入类——置顶（3）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `PinMessage` | `request_id, conversation_id, message_id` | 必须 OWNER/ADMIN（PRIVATE 双方都是 OWNER 自然通过）；当前 cid 已有 pin 数 ≥`kPinLimit(10)` → `SYSTEM_INVALID_ARGUMENT(9004)`；DAO `insert(cid, mid, pinner_uid)`，唯一约束 `(cid, mid)` 重复视幂等成功；推 `PIN_CHANGED_NOTIFY` 给所有成员（`is_pinned=true`） |
| `UnpinMessage` | `request_id, conversation_id, message_id` | 必须 OWNER/ADMIN；DAO `remove(cid, mid)`；不存在视幂等成功；推 `PIN_CHANGED_NOTIFY` 给所有成员（`is_pinned=false`） |
| `ListPinnedMessages` | `request_id, conversation_id` | 必须是会话成员；DAO `list_by_conversation(cid, limit=10)` 取 mid 列表 → `select_by_ids` 取 Message → 过滤 `status==DELETED` |

Pin 数量上限常量：`kPinLimit = 10`（对齐 spec §1 调研：微信 / Telegram 单会话置顶 10 条）。

#### 2.2.5 写入类——删除（2，仅自己 timeline）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `DeleteMessages` | `request_id, conversation_id, message_ids[]≤100` | 必须是会话成员；DAO `user_timeline.delete_by_message_ids(uid, cid, mids)`；不动 message 主表；不推 push（仅影响调用方） |
| `ClearConversation` | `request_id, conversation_id` | 必须是会话成员；DAO `user_timeline.delete_by_conversation(uid, cid)`；不推 push |

#### 2.2.6 已读 / 幂等（2）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `UpdateReadAck` | `request_id, conversation_id, seq_id` | **服务端送达 ACK**（与 Conversation.MarkRead 区分：MarkRead = 用户已读水位；UpdateReadAck = 客户端确认收到）；写 `conversation_member._last_ack_seq` GREATEST 原子推进（DAO 已有 `_atomic_advance_seq("last_ack_seq", ...)`）；用于 Push 服务 unacked 缓冲清理。proto 中删 `user_id`，从 metadata 取 |
| `SelectByClientMsgId` | `request_id, client_msg_id` | **内部接口**，给 Transmite 幂等去重备用兜底（主路径已切 Redis SETNX，但 race 边缘情况仍走 DB）；`auth.user_id == "__system__"` 放行；DAO `select_by_client_msg`；返回 Message 或空。proto 中删 `user_id`，从 metadata 取（内部调用方传 `__system__`） |

### 2.3 NotifyMessage proto 改动

```protobuf
// proto/push/notify.proto
enum NotifyType {
    FRIEND_ADD_APPLY_NOTIFY = 0;
    FRIEND_ADD_PROCESS_NOTIFY = 1;
    CONVERSATION_CREATE_NOTIFY = 2;
    CHAT_MESSAGE_NOTIFY = 3;
    FRIEND_REMOVE_NOTIFY = 4;
    MESSAGE_RECALLED_NOTIFY = 5;
    PRESENCE_CHANGE_NOTIFY = 6;
    TYPING_NOTIFY = 7;
    REACTION_CHANGED_NOTIFY = 8;       // 新增
    PIN_CHANGED_NOTIFY = 9;            // 新增
    READ_RECEIPT_NOTIFY = 10;          // 新增（PRIVATE 已读回执；GROUP 暂不推）
    CLIENT_AUTH = 49;
    MSG_PUSH_ACK = 50;
    CLIENT_HEARTBEAT = 51;
}

message NotifyReactionChanged {
    string conversation_id = 1;
    int64 message_id = 2;
    string actor_user_id = 3;
    string emoji = 4;
    bool added = 5;                     // true=AddReaction, false=RemoveReaction
}

message NotifyPinChanged {
    string conversation_id = 1;
    int64 message_id = 2;
    string actor_user_id = 3;
    bool is_pinned = 4;                 // true=Pin, false=Unpin
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
        NotifyReactionChanged reaction_changed = 15;    // 新增
        NotifyPinChanged pin_changed = 16;              // 新增
        NotifyReadReceipt read_receipt = 17;            // 新增
    }
}
```

### 2.4 PushToUserReq 强类型化

```protobuf
// proto/push/push_service.proto
message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    chatnow.push.NotifyMessage notify = 3;     // 由 bytes 改强类型
    optional uint64 user_seq = 4;
}
```

`PushBatchReq` 同步改：`bytes notify_payload` → `chatnow.push.NotifyMessage notify`。

### 2.5 错误码使用约定（spec §5.4）

| 错误码 | 触发场景 |
|---|---|
| `MESSAGE_NOT_FOUND` (4001) | 任何 RPC 中 message_id 不存在或 status=DELETED |
| `MESSAGE_RECALL_TIMEOUT` (4002) | 本人撤回但已超 120s 且非 OWNER/ADMIN |
| `MESSAGE_ALREADY_RECALLED` (4003) | 重复撤回（status=RECALLED） |
| `MESSAGE_CONTENT_INVALID` (4004) | emoji 超长 / message_ids 越界 / keyword 空 |
| `CONVERSATION_NOT_MEMBER` (3002) | caller 非会话成员 |
| `CONVERSATION_NO_PERMISSION` (3003) | OWNER/ADMIN 才能做的操作（撤回他人 / Pin / Unpin） |
| `SYSTEM_INVALID_ARGUMENT` (9004) | Pin 数 >10 / limit 越界 / before_seq=0 |
| `SYSTEM_UNAVAILABLE` (9002) | 下游 Identity / Media / Conversation 不可达；MQ publish 失败已 fail-soft 不抛 |

`HANDLE_RPC` 宏统一处理：`extract_auth + LogContext::set + ResponseHeader 写入 + ServiceError 捕获 + LogContext::clear`。handler 内一律 `throw ServiceError(code, msg)`，不再手填 `set_success(false) / set_errmsg(...)`。

---

## 3. 服务类与 handler 模式

### 3.1 类替换

- **新类**：`namespace chatnow::message { class MessageServiceImpl : public chatnow::message::MessageService }`
- **旧类** `MessageServiceImpl : public chatnow::MsgStorageService` 整体删除（与 Identity / Relationship / Conversation 同模式，不留兼容壳）

### 3.2 handler 范式（Recall 为例）

```cpp
void RecallMessage(::google::protobuf::RpcController* cntl_base,
                   const RecallMessageReq* req,
                   RecallMessageRsp* rsp,
                   ::google::protobuf::Closure* done) override {
    HANDLE_RPC(cntl, req, rsp, {
        auto msg = _mysql_msg->select_by_id(req->message_id());
        if (!msg) throw ServiceError(kMessageNotFound, "mid not found");
        if (msg->conversation_id() != req->conversation_id())
            throw ServiceError(kMessageNotFound, "cid mismatch");
        if (msg->status() == MessageStatus::RECALLED)
            throw ServiceError(kMessageAlreadyRecalled, "already recalled");

        auto role = _conv_role_(req->conversation_id(), auth.user_id);
        bool is_admin = (role == OWNER || role == ADMIN);
        bool is_self  = (msg->user_id() == auth.user_id);
        int64_t age_ms = now_ms() - msg->created_at_ms();
        if (!is_admin) {
            if (!is_self) throw ServiceError(kConversationNoPermission, "not msg author");
            if (age_ms >= kRecallTimeoutMs)
                throw ServiceError(kMessageRecallTimeout, "exceed 120s window");
        }

        if (!_mysql_msg->update_status_to_recalled(req->message_id()))
            throw ServiceError(kMessageAlreadyRecalled, "race lost or not recallable");

        publish_recalled_notify_(cntl, req->conversation_id(), req->message_id());
    });
}
```

### 3.3 内部辅助方法

| 方法 | 用途 |
|---|---|
| `bool require_member_(cid, uid)` | 直接读 `_mysql_member` DAO 的 `select_self(cid, uid)`（与 Conversation 服务共享 conversation_member 表）；不存在 / status=quit → 抛 `kConversationNotMember` |
| `MemberRole _conv_role_(cid, uid)` | 同上 DAO 路径，复用 `select_self` 的返回 role 字段；不每次跨服务 RPC，避免 Pin / Recall 高频路径每次额外两跳 |
| `void publish_chat_notify_(...)` | 已有：组装 NotifyMessage(CHAT_MESSAGE_NOTIFY) → 投 push_queue（onDBMessage 调） |
| `void publish_recalled_notify_(in_cntl, cid, mid)` | 新增：组装 NotifyMessage(MESSAGE_RECALLED_NOTIFY, NotifyMessageRecalled{cid, mid}) → 投 push_queue → 失败 ERROR + 入 push_outbox |
| `void publish_reaction_notify_(in_cntl, sender_id, mid, actor, emoji, added)` | 新增：仅推 `target_user_ids=[sender_id]`；NotifyMessage(REACTION_CHANGED_NOTIFY) |
| `void publish_pin_notify_(in_cntl, cid, mid, actor, pinned)` | 新增：推会话所有成员；NotifyMessage(PIN_CHANGED_NOTIFY) |
| `std::vector<UserInfo> fetch_user_infos_(in_cntl, uids)` | 调 IdentityService.GetMultiUserInfo；fail-soft 返回空 vector |
| `void fill_reactions_for_messages_(msgs, caller_uid)` | 批量取 message_reaction 行 → 按 mid 分组 → 每 mid 内 GROUP BY emoji 聚合 → 填 Message.reactions（含 self_reacted） |
| `void fill_pin_flag_for_messages_(cid, msgs)` | 批量调 `message_pin.list_pinned_in(cid, mids)` → 填 Message.is_pinned |
| `int64_t now_ms_()` | 当前时间戳（毫秒） |

`publish_recalled_notify_` 等所有 `publish_*_notify_` 方法均**fail-soft**：MQ 投递失败仅记 ERROR + 入 `push_outbox`，**不抛 ServiceError**——保证 RPC 主体（DB 写入）成功后，推送失败不影响业务结果。

### 3.4 跨服务 stub 切换

| 调用 | 旧 stub | 新 stub | 编译状态 |
|---|---|---|---|
| 填 sender UserInfo / reply_to.replied_sender_id | `chatnow::UserService_Stub.GetMultiUserInfo` | `chatnow::identity::IdentityService_Stub.GetMultiUserInfo` | ✅ 已迁，可编译 |
| 校验会话成员 / 取 role | （直接读 ChatSessionMember 表，已废弃） | **直接读 `conversation_member` 表**（DAO `ConversationMemberTable.select_self(cid, uid)`，与 Conversation 服务共享同一张表）；不走 RPC | ✅ DAO 已存在 |
| 取附件 mime/size 元信息 | `chatnow::FileService_Stub.GetSingleFile` / `GetMultiFile` | `chatnow::media::MediaService_Stub.DownloadFile`（循环单数） | ✅ 已迁，可编译 |
| 上传消息附件（保留 channel） | `chatnow::FileService_Stub.PutSingleFile` | `chatnow::media::MediaService_Stub.UploadFile` | ✅ 已迁，本期 message 服务无写入文件路径，stub 切换但调用点为 0 |

每次跨服务 RPC 前 `chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl)` 透传。Internal RPC（onDBMessage / onESIndexMessage 内的 GetMultiUserInfo）使用 `__system__` 用户身份。

---

## 4. 数据层改动

### 4.1 ODB schema（零改动）

| 表 | 状态 |
|---|---|
| `message` | ✅ 现状保留（已有 status / client_msg_id / reply_to_msg_id / 单附件 file_id） |
| `user_timeline` | ✅ 现状保留 |
| `message_reaction` | ✅ 现状保留（odb/message_reaction.hxx） |
| `message_pin` | ✅ 现状保留（odb/message_pin.hxx） |
| `message_attachment` | ✅ 保留不动 |
| `message_mention` | ✅ 保留不动 |

**没有新表，没有 schema 变更。**

### 4.2 DAO 新增方法

#### 4.2.1 `common/dao/mysql_message.hpp`（+4 个方法）

| 方法 | 用途 | SQL 模式 |
|---|---|---|
| `bool update_status_to_recalled(unsigned long mid)` | RecallMessage 软删 | `UPDATE message SET status=1, content_text='' WHERE id=? AND status=0`（status=0→1 race-safe） |
| `int64_t select_max_seq_by_conversation(const std::string& cid)` | SyncMessages 取 latest_seq；SeqGen 启动回填 | `SELECT MAX(seq_id) FROM message WHERE conversation_id=?` |
| `std::vector<Message> select_history(const std::string& cid, uint64_t before_seq, int limit)` | GetHistory | `WHERE conversation_id=? AND seq_id<? AND status!=2 ORDER BY seq_id DESC LIMIT ?` |
| `std::vector<Message> select_after(const std::string& cid, uint64_t after_seq, int limit)` | SyncMessages | `WHERE conversation_id=? AND seq_id>? AND status!=2 ORDER BY seq_id ASC LIMIT ?` |

注：`select_by_id` / `select_by_ids` / `select_by_client_msg` 已存在，复用。

#### 4.2.2 `common/dao/mysql_user_timeline.hpp`（+3 个方法）

| 方法 | 用途 |
|---|---|
| `int delete_by_message_ids(uid, cid, std::vector<uint64> mids)` | DeleteMessages 批量删 |
| `int delete_by_conversation(uid, cid)` | ClearConversation 整会话清 |
| `std::vector<std::pair<std::string,uint64_t>> select_max_user_seq_per_user()` | SeqGen 启动回填 user_seq；返回 (uid, MAX(user_seq)) 列表 |

注：`unread_count_by_seq` / `select_recent` 等现有方法保留。

#### 4.2.3 新文件 `common/dao/mysql_message_reaction.hpp`（对应已有 odb/message_reaction.hxx）

| 方法 | 用途 |
|---|---|
| `bool insert(mid, uid, emoji)` | AddReaction，唯一索引冲突视幂等成功 |
| `bool remove(mid, uid, emoji)` | RemoveReaction，不存在返回 true |
| `std::vector<ReactionRow> select_by_message(uint64_t mid)` | GetReactions 取所有行 |
| `std::vector<ReactionRow> select_by_messages(std::vector<uint64_t> mids)` | GetHistory / SyncMessages 批量填充 |

服务层在 handler 内做 `GROUP BY emoji COUNT(*) + 取前 3 个 user_id + self_reacted` 聚合（不在 DAO 层混入业务）。`ReactionRow = struct { uint64_t message_id; std::string user_id; std::string emoji; uint64_t id; }`，`id` 用于"最近 3 个"的稳定排序。

#### 4.2.4 新文件 `common/dao/mysql_message_pin.hpp`（对应已有 odb/message_pin.hxx）

| 方法 | 用途 |
|---|---|
| `bool insert(cid, mid, pinner_uid)` | PinMessage，唯一索引冲突视幂等成功 |
| `bool remove(cid, mid)` | UnpinMessage，不存在返回 true |
| `int count_by_conversation(cid)` | Pin 上限校验（10） |
| `std::vector<uint64_t> list_by_conversation(cid, int limit=10)` | ListPinnedMessages 取 mid 列表 |
| `std::vector<uint64_t> list_pinned_in(cid, std::vector<uint64_t> mids)` | 批量返回该 mid 集合中 pinned 的子集，给 fill_pin_flag_for_messages_ 用 |

### 4.3 Redis 数据（沿用，不动结构）

| Key | 用途 | 已有 |
|---|---|---|
| `im:seq:{cid}` | SeqGen.session_seq | ✅ |
| `im:user_seq:{uid}` | SeqGen.user_seq | ✅ |
| `im:dedup:{user_id}:{client_msg_id}` | Transmite 幂等 SETNX | ✅ |
| `im:push:outbox` | push_queue 投递失败兜底 ZSET | ✅ |
| `im:es:outbox` | es_index_exchange 投递失败兜底 ZSET | ✅ |

**本期新增**：无。

`SeqGen.backfill_session` / `backfill_user` 实现切换为 Lua 原子（reliability spec §1.3）：

```lua
local cur = redis.call('GET', KEYS[1])
if not cur or tonumber(cur) < tonumber(ARGV[1]) then
    redis.call('SET', KEYS[1], ARGV[1])
    return 1
end
return 0
```

### 4.4 ES 索引（沿用）

`ESMessage` 类不动，`index="messages"` 字面量保留。`onESIndexMessage` 反序列化包名切到 `chatnow::message::internal::ESIndexEvent`。文档片段 schema 不变（`_id=message_id, _source={conversation_id, sender_id, content_text, created_at_ms, seq_id, message_type}`）。

---

## 5. 服务结构 / Builder / main / CMake

### 5.1 类与 Builder

- `MessageServiceImpl`：实现 15 RPC，注入 mysql_msg / mysql_user_timeline / mysql_member（角色查询） / mysql_reaction / mysql_pin / es_msg / seq_gen / push_publisher / push_outbox / es_publisher / es_outbox / mm_channels（identity / media / conversation 三个下游）/ cfg
- `MessageServer`：持有 brpc::Server + MQClient + reaper threads + Registry，停服顺序与 Push 同模式（reaper → MQ ev → brpc Stop/Join → delete impl）
- `MessageServerBuilder`：分步骤 make_*_object，最终 `make_rpc_object` 内部完成：
  1. brpc::Server.AddService(impl, SERVER_OWNS_SERVICE)
  2. brpc::Server.Start(port)
  3. **SeqGen 启动回填**（先回填，再订阅 MQ）
  4. 启动 push_outbox_reaper / es_outbox_reaper
  5. MQ subscribe（onDBMessage / onESIndexMessage）

### 5.2 SeqGen 启动回填实现

```cpp
void MessageServerBuilder::backfill_seq_from_db_() {
    // session seq
    auto session_pairs = _mysql_msg->select_max_seq_per_conversation();
    int s_count = 0;
    for (auto &[cid, max_seq] : session_pairs) {
        if (_seq_gen->backfill_session(cid, max_seq + 1)) ++s_count;
    }
    // user seq
    auto user_pairs = _mysql_user_timeline->select_max_user_seq_per_user();
    int u_count = 0;
    for (auto &[uid, max_seq] : user_pairs) {
        if (_seq_gen->backfill_user(uid, max_seq + 1)) ++u_count;
    }
    LOG_INFO("seqgen backfill done sessions={}/{} users={}/{}",
             s_count, session_pairs.size(), u_count, user_pairs.size());
}
```

`select_max_seq_per_conversation` 走覆盖索引 `(conversation_id, seq_id)`：`SELECT conversation_id, MAX(seq_id) FROM message GROUP BY conversation_id`。百万消息千会话级别预计 < 5s（reliability spec §1.4）。

### 5.3 文件改动清单

#### 5.3.1 新增

| 路径 | 内容 |
|---|---|
| `common/dao/mysql_message_reaction.hpp` | MessageReactionTable DAO |
| `common/dao/mysql_message_pin.hpp` | MessagePinTable DAO |

#### 5.3.2 修改

| 路径 | 改动 |
|---|---|
| `proto/message/message_service.proto` | 删 user_id/session_id（2 处）；加 cc_generic_services；引用全限定 |
| `proto/message/message_internal.proto` | 加 cc_generic_services；引用全限定 |
| `proto/push/notify.proto` | 加 3 NotifyType + 3 NotifyXxx + oneof 加 3 分支；加 cc_generic_services；引用全限定 |
| `proto/push/push_service.proto` | bytes notify_payload → NotifyMessage notify；加 cc_generic_services；引用全限定 |
| `common/dao/mysql_message.hpp` | +4 方法 |
| `common/dao/mysql_user_timeline.hpp` | +3 方法 |
| `common/dao/data_redis.hpp` | SeqGen.backfill_session / backfill_user 改 Lua 原子 |
| `common/error/error_codes.hpp` | 检查 4001-4004 / 3003 已存在；按需补 |
| `message/source/message_server.h` | **完整重写**：MessageServiceImpl 15 RPC + Builder + SeqGen 回填 + 跨服务 stub 改包；旧 chatnow::MsgStorageService 实现整体删除 |
| `message/source/message_server.cc` | main 沿用；服务名 / etcd 路径改 |
| `message/CMakeLists.txt` | proto_files 加 message_service / message_internal / message_types / push notify / push_service / identity / media / conversation 下游 stub |
| `conf/message_server.conf` | 服务名 message_service；下游 service 名 gflag 同步 |
| `gateway/source/gateway_server.h` | 5 个旧 message handler 切 stub + RPC；删旧 GetUnreadCount / GetOfflineMsg；新增 11 个 handler |
| `docker-compose.yml` | message_server 服务名不变；gflag 同步；depends_on 含 etcd / mysql / redis / rabbitmq / elasticsearch |

#### 5.3.3 删除

| 路径 | 时机 |
|---|---|
| `proto/message.proto` 旧 flat proto（如还存在） | Gateway 切换完 + 全仓 grep `chatnow::MsgStorage` 零命中后删 |
| 顶层 CMakeLists.txt | `add_subdirectory(message)` 不需要改名 |

### 5.4 Gateway HTTP handler 切换矩阵

| HTTP 路由 | 旧 | 新 |
|---|---|---|
| `/service/message/get_history` | `MsgStorageService_Stub.GetHistoryMsg` | `MessageService_Stub.GetHistory` |
| `/service/message/get_recent` | `MsgStorageService_Stub.GetRecentMsg` | `MessageService_Stub.SyncMessages` |
| `/service/message/search` | `MsgStorageService_Stub.MsgSearch` | `MessageService_Stub.SearchMessages` |
| `/service/message/get_offline`（删除）| `MsgStorageService_Stub.GetOfflineMsg` | **废弃**（前端走 SyncMessages with after_seq=0） |
| `/service/message/get_unread`（删除）| `MsgStorageService_Stub.GetUnreadCount` | **废弃**（前端走 Conversation.SelfMemberInfo.unread_count） |
| `/service/message/recall`（新） | — | `MessageService_Stub.RecallMessage` |
| `/service/message/reaction/add`（新） | — | `MessageService_Stub.AddReaction` |
| `/service/message/reaction/remove`（新） | — | `MessageService_Stub.RemoveReaction` |
| `/service/message/reaction/list`（新） | — | `MessageService_Stub.GetReactions` |
| `/service/message/pin`（新） | — | `MessageService_Stub.PinMessage` |
| `/service/message/unpin`（新） | — | `MessageService_Stub.UnpinMessage` |
| `/service/message/list_pinned`（新） | — | `MessageService_Stub.ListPinnedMessages` |
| `/service/message/delete`（新） | — | `MessageService_Stub.DeleteMessages` |
| `/service/message/clear`（新） | — | `MessageService_Stub.ClearConversation` |
| `/service/message/get_by_id`（新） | — | `MessageService_Stub.GetMessagesById` |

每个 handler：
- `apply_auth_to_brpc(request, cntl, _auth)` 写 metadata
- 错误响应改写 `header.error_code` + `header.error_message`，不再用 `set_success / set_errmsg`

`SelectByClientMsgId` / `UpdateReadAck` 是内部接口，**不暴露 HTTP**。

---

## 6. 验收标准

每条都需在实施 plan 里展开成可测 step：

1. **proto 清理验证**：
   - `grep "optional string user_id\|optional string session_id" proto/message/*.proto proto/push/*.proto` 零命中
   - `grep "cc_generic_services" proto/message/*.proto proto/push/*.proto` 5 处全部命中
   - 引用全限定（无非全限定 ResponseHeader / UserInfo / MessagePreview）

2. **NotifyMessage 强类型验证**：
   - `proto/push/push_service.proto` 中 `bytes notify_payload` 字段彻底删除
   - `proto/push/notify.proto` 中 NotifyType 枚举包含 REACTION_CHANGED_NOTIFY / PIN_CHANGED_NOTIFY / READ_RECEIPT_NOTIFY 三个新值
   - NotifyMessage.oneof 含三个新分支

3. **15 个 RPC handler 全部使用 HANDLE_RPC 宏**；从 metadata 取 user_id（grep `req->user_id()` 应零命中或仅在内部 SelectByClientMsgId / UpdateReadAck 兼容路径出现）

4. **Recall 三种路径都有测试**：
   - 本人 < 120s：成功 + 推 NotifyMessageRecalled
   - 本人 ≥ 120s 且非 ADMIN：返回 MESSAGE_RECALL_TIMEOUT(4002)
   - 群 OWNER/ADMIN 任意时间：成功
   - 重复撤回：返回 MESSAGE_ALREADY_RECALLED(4003)

5. **Reaction 唯一索引幂等**：相同 (mid, uid, emoji) 重复 AddReaction 不报错；GetReactions 返回单分组 count=1

6. **Pin 数量限制**：第 11 次 PinMessage 返回 SYSTEM_INVALID_ARGUMENT；UnpinMessage 后可再 Pin

7. **Delete / Clear 范围**：调用方 user_timeline 行被删；其他成员的 user_timeline 不受影响；message 主表行未变

8. **SeqGen 启动回填**：清空 Redis im:seq:{cid} → 启动 message_server → 回填日志含 `seqgen backfill done sessions=... users=...`；新消息 seq 从 max+1 开始

9. **MQ consumer 包名切换**：`onDBMessage` / `onESIndexMessage` 反序列化 `chatnow::message::internal::InternalMessage` / `ESIndexEvent` 成功；旧 chatnow::InternalMessage 已不存在

10. **跨服务 stub 切换**：`grep "chatnow::UserService_Stub\|chatnow::FileService_Stub\|chatnow::MsgStorageService_Stub"` 在 message/ 目录零命中

11. **Gateway 路由切换**：`grep MsgStorageService_Stub gateway/` 零命中；新增 11 个 message handler 可被 HTTP 调通

12. **旧 proto 删除**：`proto/message.proto`（如还存在）+ 全仓 `grep chatnow::MsgStorage` 零命中后删除

---

## 7. 编译阻塞 / 兼容性

### 7.1 编译阻塞依赖

| 编译目标 | 依赖 | 状态 |
|---|---|---|
| `message_server` | identity proto ✅ / media proto ✅ / conversation proto ✅ / push proto（本期改）✅ | 本期编译可过 |
| `gateway_server` | 上述 + transmite proto（仍是旧 cc_generic_services 不全）❌ | 本期 message handler 切换可写完，但 gateway 整体 build 仍被 transmite 阻塞（与 Conversation 现状同质），等 Transmite 迁移期一起过 |
| `transmite_server` | 同上 | Transmite 迁移期编译 |
| `push_server` | NotifyMessage 强类型 wire 兼容；push proto 改了（cc_generic_services + 全限定）→ Push 现有实现需要小修 stub include 路径 | Push 迁移期统一过 |

**结论**：本期实施后 `message_server` 单独可编译。Gateway / Transmite / Push 整体编译验证留给各自迁移期。

### 7.2 兼容性

- **客户端**：本项目客户端将整体重构，不考虑旧客户端兼容（用户在 brainstorming 中明确）
- **DB 表变更**：无 schema 变更
- **MQ 拓扑**：完全不变
- **InternalMessage 包名变更**：deploy 必须先停 Transmite + Message 一起换再起，否则 Transmite 还在写老 `chatnow::InternalMessage`、Message 反序列化新包名会失败。开发阶段 docker-compose 重启所有服务即可
- **NotifyMessage 强类型 vs Push 旧实现**：本期 Message 投 push_queue 的 payload 是新包名 `chatnow::push::NotifyMessage` 的 SerializeAsString。Push 服务现有实现仍 deserialize 为旧包名 `chatnow::NotifyMessage`，二者 wire format 一致（同名 message 同 tag），proto3 二进制兼容，**中间态可上线**。Push 迁移 spec 落地后包名对齐

---

## 8. 工作量估算

| 模块 | 行数估 |
|---|---|
| proto 改动（4 个文件） | ~80 |
| MessageReactionTable + MessagePinTable DAO（新文件） | ~200 |
| mysql_message / mysql_user_timeline / data_redis（新增方法） | ~150 |
| MessageServiceImpl 15 个 handler | ~900 |
| Server / Builder / main / SeqGen 回填 | ~250 |
| MQ consumer 包名切换 + Notify 强类型 publish | ~50 |
| Gateway 11 新 handler + 5 旧切换 + 2 删除 | ~350 |
| **合计** | **~1980** |

---

## 9. 后续工作（不在本 spec 范围）

- **Transmite 服务迁移**（独立 spec）：InternalMessage 包名切换 publisher 端；stub 改名；ResponseHeader 切换；GetTransmitTarget RPC 处理 Member 缓存
- **Push + Presence 服务迁移**（独立 spec）：Device 模型 + WS 设备级路由 + JWT did 字段 + Identity Login.device_id + ListDevices/RevokeDevice + NotifyMessage 强类型反序列化路径切换 + CONVERSATION_DISMISSED_NOTIFY 补 proto + Conversation 推送
- **MarkRead 水位缓存改造**（独立 spec，本期之后下一项）：Redis im:read:{cid}:{uid} cache + write-behind flush worker；ListConversations 走缓存
- **多实例 Message 服务同会话 seq 严格有序**（已有 spec `2026-05-14-mq-quorum-and-seq-ordering-design.md`，独立实施）
- **Pin 数量上限 / 撤回时间窗成可配**（产品需求驱动后再做）

---

## 10. 实施记录

> **实施落地后填写。** 模仿 `2026-05-15-relationship-service-migration-design.md` §11 / `2026-05-16-conversation-service-migration-design.md` §11 同模式。
>
> 至少包含：状态总览表 / commit 序列 / 横切 hotfix 列表 / 关键设计选择固化记录 / 已知阻塞 / 给下一位 Agent 的接手清单 / 结构性约束。
