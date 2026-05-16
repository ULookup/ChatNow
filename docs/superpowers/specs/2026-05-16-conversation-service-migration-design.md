# Conversation 服务迁移设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-16
> **范围**: 旧 `chatsession/` 服务（`ChatSessionServiceImpl : public ChatSessionService`）迁到新 proto `chatnow.conversation.ConversationService`
> **基线**: `proto/conversation/conversation_service.proto` + `2026-05-14-cross-cutting-architecture-design.md` §2.6 §2.8 §5.5 + `2026-05-14-service-migration-design.md` §3.3 + `2026-05-15-relationship-service-migration-design.md`（同模式）
> **前置**: Identity / Media / Relationship 已迁移；Message 尚未迁移（本期编译阻塞与 Relationship 当前同质）

---

## 1. 范围与目标

把 `chatsession/source/chatsession_server.h` 从老 `ChatSessionServiceImpl : public ChatSessionService` 迁到 `ConversationServiceImpl : public chatnow::conversation::ConversationService`。同步：

- **目录改名**：`chatsession/` → `conversation/`
- **ODB 实体改名**：`chat_session.hxx` → `conversation.hxx`、`chat_session_member.hxx` → `conversation_member.hxx`；表名 `chat_session` → `conversation`、`chat_session_member` → `conversation_member`；类名、枚举名同步（开发阶段，不需要 ALTER TABLE 迁移脚本）
- **DAO 改名**：`mysql_chat_session*.hpp` → `mysql_conversation*.hpp`；`ChatSessionTable` → `ConversationTable`，`ChatSessionMemberTable` → `ConversationMemberTable`
- 全 18 个 RPC 落实（含 4 个新增：`DismissConversation` / `SaveDraft` / `MarkRead` 语义新版 / `GetUnreadCount` 合并入 `SelfMemberInfo.unread_count`）
- 鉴权字段全走 metadata，proto 业务体清理 `optional user_id / session_id`（约 36 处）
- 响应模式从 `success/errmsg` 改为 `ResponseHeader`，handler 一律用 `HANDLE_RPC` 宏 + `throw ServiceError(code, msg)`
- 群头像与 Identity Avatar 同模式：`UpdateConversation` 接受 `avatar_file_id`，服务端转 public bucket URL 写库
- 跨服务 stub：`UserService_Stub` → `chatnow::identity::IdentityService_Stub`（已迁，可编译）；`FileService_Stub` 调用全部删除（客户端直传）；`MsgStorageService_Stub.GetRecentMsg` → `chatnow::message::MessageService_Stub.SyncMessages`（**Message 未迁，本期写新 stub 名，编译阻塞与 Relationship 同质**）
- Gateway 16 个 chatsession HTTP handler（14 改 + 2 新增 dismiss/save_draft）跟进切换到新 stub + 新 RPC

明确**不做**（YAGNI）：

- ❌ 不动 Conversation Redis Members 缓存语义（仅 key 前缀 `chatsession:` → `conversation:`）
- ❌ 不为 unread_count 引入实时计数（每次 ListConversations 现算：`max_seq - last_read_seq`，`max_seq` 由 SyncMessages 返回；Message 迁移期前先返回 0 + TODO）
- ❌ 不实现 SaveDraft 跨设备同步（仅服务端持久；客户端可走 ListConversations 拉回）
- ❌ 不重命名 ES 索引（`ESChatSession` 类改名 `ESConversation`，但内部 index 字符串仍是 `"chat_session"`）
- ❌ 不改客户端 HTTP 路径（沿用 `/service/chatsession/*`，与 Relationship 保留 `/service/friend/*` 同策略）
- ❌ 不引入新 Redis 数据结构

---

## 2. 服务接口与 proto 字段

### 2.1 proto 改动（`proto/conversation/conversation_service.proto`）

- 全 18 个 Req 删除 `optional string user_id` / `optional string session_id` 字段（共 36 处，与横切 spec §2.8 一致）
- 加 `option cc_generic_services = true;`（与 Identity / Relationship 一致）
- 引用全限定：`ResponseHeader` → `chatnow.common.ResponseHeader`；`PageRequest` / `PageResponse` → `chatnow.common.*`；`UserInfo` → `chatnow.identity.UserInfo`；`MessagePreview` → `chatnow.message.MessagePreview`
- 业务字段保留：所有 conversation_id / member_ids / role 字段不变

### 2.2 RPC 矩阵（18 个）

#### 2.2.1 读取类（5）

| RPC | 关键 Req | 关键 Rsp | 备注 |
|---|---|---|---|
| `ListConversations` | `request_id, page` | `header, conversations[], page` | 每个 Conversation 含 `SelfMemberInfo`；`last_message` 调 `MessageService.SyncMessages(after_seq=last_read_seq, limit=1)` 取最新预览 |
| `GetConversation` | `request_id, conversation_id` | `header, conversation` | 含 SelfMemberInfo |
| `ListMembers` | `request_id, conversation_id, page` | `header, members[](MemberItem), page` | UserInfo 调 `IdentityService.GetMultiUserInfo` 批量填充 |
| `SearchConversations` | `request_id, search_key` | `header, conversations[]` | 仅返回当前用户参与的 |
| `GetMemberIds` | `request_id, conversation_id` | `header, member_ids[]` | **内部接口**（Transmite 依赖）；`auth.user_id == "__system__"` 时放行不做成员校验 |

#### 2.2.2 会话生命周期（4）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `CreateConversation` | `request_id, type, name?, avatar_url?, member_ids[]` | PRIVATE 强制 `member_ids.size==1`，`conversation_id` 用 `(min_uid, max_uid)` 哈希做幂等；GROUP 用雪花。caller=OWNER，其它=MEMBER |
| `UpdateConversation` | `request_id, conversation_id, name?, avatar_url?(file_id), description?` | 仅 OWNER/ADMIN；任一 optional 非空才写；`avatar_url` 字段实际承载 `avatar_file_id`，服务端转换成 `<media.public_url_prefix>/group_avatar/{file_id}` 写 DB |
| `DismissConversation`（新增）| `request_id, conversation_id` | 仅 OWNER；`status=DISMISSED`；软删；推 `CONVERSATION_DISMISSED_NOTIFY` 给所有原成员（推送由本服务发到 Push MQ；若 MQ 不可达 fail-soft + ERROR 日志） |
| `QuitConversation` | `request_id, conversation_id` | 删自己的 ConversationMember 行；OWNER 拒（`CONVERSATION_NO_PERMISSION` "owner must transfer first"）；`member_count--` |

#### 2.2.3 成员管理（4）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `AddMembers` | `conversation_id, member_ids[]` | 仅 OWNER/ADMIN；GROUP 才允许；去重已有；超 `group_member_limit` → `CONVERSATION_MEMBER_LIMIT` |
| `RemoveMembers` | `conversation_id, member_ids[]` | 仅 OWNER/ADMIN；不能踢自己；OWNER 不能被踢 |
| `TransferOwner` | `conversation_id, new_owner_id` | 仅 OWNER；new_owner 必须是成员；旧 OWNER 降为 ADMIN |
| `ChangeMemberRole` | `conversation_id, target_user_id, role(MEMBER/ADMIN)` | 仅 OWNER；不能改 OWNER 的角色；不能升任另一个为 OWNER（用 TransferOwner） |

#### 2.2.4 自身偏好（4，每个返回 `SelfMemberInfo`）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `SetMute` | `conversation_id, mute(bool)` | 写 `_is_muted` |
| `SetPin` | `conversation_id, pin(bool)` | 写 `_is_pinned` + `_pin_time_ms` |
| `SetVisible` | `conversation_id, visible(bool)` | 写 `_is_visible`；隐藏会话不在 ListConversations 返回 |
| `SaveDraft`（新增）| `conversation_id, draft` | 写 `_draft` nullable<string> |

#### 2.2.5 已读（1，新增）

| RPC | 关键 Req | 备注 |
|---|---|---|
| `MarkRead` | `conversation_id, last_read_seq` | `_last_read_seq = max(old, new)` 防回退；PRIVATE 推 `READ_RECEIPT_NOTIFY` 给对端，GROUP 暂不推（YAGNI） |

### 2.3 错误码映射（spec §5.4）

| 错误码 | 触发场景 |
|---|---|
| `CONVERSATION_NOT_FOUND` (3001) | conversation_id 不存在 |
| `CONVERSATION_NOT_MEMBER` (3002) | caller 非成员 |
| `CONVERSATION_NO_PERMISSION` (3003) | 角色不足（普通成员改群名 / 非 OWNER 解散 / 非 OWNER/ADMIN 加人 / OWNER QuitConversation 等） |
| `CONVERSATION_MEMBER_LIMIT` (3004) | GROUP 加人超员 |
| `SYSTEM_INVALID_ARGUMENT` (9004) | PRIVATE 不接受成员管理类 RPC；PRIVATE 创建参数非法等 |
| `AUTH_USER_NOT_FOUND` (1004) | 成员管理时 target_user_id 不存在（IdentityService 返回空 map 时不主动校验，沿用现状） |
| `SYSTEM_UNAVAILABLE` (9002) | 下游 Identity / Media / Message 不可达。**调用类填充**（`last_message` / `MemberItem.user_info`）走 fail-soft：留空 + ERROR 日志，不抛 ServiceError |

`HANDLE_RPC` 宏统一处理：`extract_auth + LogContext::set + ResponseHeader 写入 + ServiceError 捕获 + LogContext::clear`。handler 内一律 `throw ServiceError(code, msg)`，不再手填 `set_success(false) / set_errmsg(...)`。

---

## 3. 服务类与 handler 模式

### 3.1 类替换

- **新类**：`namespace chatnow::conversation { class ConversationServiceImpl : public chatnow::conversation::ConversationService }`
- **旧类** `ChatSessionServiceImpl : public ChatSessionService` 整体删除（不留兼容壳，与 Identity / Relationship 同模式）

### 3.2 handler 范式

```cpp
void CreateConversation(::google::protobuf::RpcController* cntl_base,
                        const CreateConversationReq* req,
                        CreateConversationRsp* rsp,
                        ::google::protobuf::Closure* done) override {
    HANDLE_RPC(cntl, req, rsp, {
        if (req->type() == ConversationType::PRIVATE && req->member_ids_size() != 1)
            throw ServiceError(ErrorCode::SYSTEM_INVALID_ARGUMENT, "private requires exactly 1 peer");
        if (req->type() == ConversationType::GROUP && req->member_ids_size() == 0)
            throw ServiceError(ErrorCode::SYSTEM_INVALID_ARGUMENT, "group needs initial members");
        // ... 业务：生成 conversation_id / 写 ConversationTable / 写 ConversationMemberTable / 失效缓存 / ES upsert
    });
}
```

### 3.3 内部辅助（私有方法）

| 方法 | 用途 |
|---|---|
| `bool require_member_(cid, uid)` | 校验 caller 是会话成员，不是 → 抛 `CONVERSATION_NOT_MEMBER` |
| `MemberRole get_role_(cid, uid)` | 读 conversation_member 行，给权限校验用 |
| `void invalidate_members_cache_(cid)` | 写入类 RPC 后 `del conversation:members:{cid}` |
| `std::vector<UserInfo> fetch_user_infos_(in_cntl, uids)` | 调 IdentityService.GetMultiUserInfo；fail-soft 返回空 vector |
| `std::optional<MessagePreview> fetch_last_message_(in_cntl, cid, after_seq)` | 调 MessageService.SyncMessages；fail-soft 返回 nullopt |
| `std::string private_conversation_id_(uid_a, uid_b)` | `min(a,b) + ":" + max(a,b)` 哈希得固定 ID（PRIVATE 幂等） |
| `std::string avatar_file_id_to_url_(file_id)` | `<media.public_url_prefix>/group_avatar/{file_id}` |

---

## 4. 数据层改动

### 4.1 ODB 实体重命名

| 旧文件 | 新文件 | 旧表 | 新表 |
|---|---|---|---|
| `odb/chat_session.hxx` | `odb/conversation.hxx` | `chat_session` | `conversation` |
| `odb/chat_session_member.hxx` | `odb/conversation_member.hxx` | `chat_session_member` | `conversation_member` |

类与枚举改名：

- `ChatSession` → `Conversation`
- `ChatSessionMember` → `ConversationMember`
- `ChatSessionStatus` → `ConversationStatus`（值 `NORMAL / ARCHIVED / DISMISSED / BANNED` 不变）
- `ChatSessionType` → `ConversationType`（与 proto 对齐：`PRIVATE / GROUP / CHANNEL`，CHANNEL 仅占位）
- 字段成员变量保留 `_` 前缀风格：`_chat_session_id` → `_conversation_id` 等

字段已确认存在（无需新增）：

- `ConversationMember._draft (nullable<std::string>)` ✅
- `ConversationMember._last_read_seq (uint64)` ✅
- `_is_pinned`, `_pin_time_ms`, `_is_muted`, `_is_visible` ✅

### 4.2 Schema 生成

- 走 `--generate-schema` 自动生成（与项目惯例一致），不写 `sql/V*.sql`
- 开发阶段，`docker-compose down -v && docker-compose up` 重建即可
- protoc + odb codegen 在 `cmake --build` 时自动触发

### 4.3 DAO 重命名

| 旧文件 | 新文件 | 旧类 | 新类 |
|---|---|---|---|
| `common/dao/mysql_chat_session.hpp` | `common/dao/mysql_conversation.hpp` | `ChatSessionTable` | `ConversationTable` |
| `common/dao/mysql_chat_session_member.hpp` | `common/dao/mysql_conversation_member.hpp` | `ChatSessionMemberTable` | `ConversationMemberTable` |

接口签名保留现有方法（`insert/select/update/remove/list_by_user/list_members/...`）只改类名 + 实体类型。**新增 4 个 DAO 方法**：

| 方法 | 用于 |
|---|---|
| `bool ConversationTable::update_status(cid, ConversationStatus s)` | DismissConversation |
| `bool ConversationMemberTable::update_draft(cid, uid, std::string)` | SaveDraft |
| `bool ConversationMemberTable::update_last_read_seq(cid, uid, uint64 seq)` | MarkRead（带 `WHERE last_read_seq < new_seq` 防回退） |
| `bool ConversationMemberTable::update_role(cid, uid, MemberRole r)` | ChangeMemberRole / TransferOwner |

每个方法**单条 UPDATE**，不引入事务复合操作。

### 4.4 Redis Members 缓存

- 沿用既有结构（SET / HASH 不变），只改 key 前缀：`chatsession:members:{cid}` → `conversation:members:{cid}`
- 失效策略：写入类 RPC（CreateConversation / AddMembers / RemoveMembers / QuitConversation / DismissConversation / TransferOwner）改完 DB 后 `del key`，下次 cache-aside 重建
- 不引入新结构

### 4.5 ES 索引

- `ESChatSession` 类**改名** `ESConversation`，内部 `index` 字符串字面量仍为 `"chat_session"`（不重建索引）
- 字段映射不变
- CreateConversation / UpdateConversation / DismissConversation 时同步 upsert

### 4.6 跨服务 stub 切换

| 调用 | 旧 stub / RPC | 新 stub / RPC | 编译状态 |
|---|---|---|---|
| 填 MemberItem.user_info | `UserService_Stub.GetMultiUserInfo` | `chatnow::identity::IdentityService_Stub.GetMultiUserInfo` | ✅ 已迁，可编译 |
| 上传群头像 | `FileService_Stub.PutSingleFile` | **删除调用**：客户端直传后传 file_id | ✅ |
| 取 last_message / max_seq | `MsgStorageService_Stub.GetRecentMsg` | `chatnow::message::MessageService_Stub.SyncMessages(after_seq=last_read_seq, limit=1)` | ❌ 等 Message 迁移期一起编译 |

每次跨服务 RPC 调用前 `chatnow::auth::forward_auth_metadata(in_cntl, &out_cntl)` 透传。

---

## 5. 服务结构与 Builder

### 5.1 新增文件

| 路径 | 内容 |
|---|---|
| `odb/conversation.hxx` | Conversation 实体（替代 chat_session.hxx）|
| `odb/conversation_member.hxx` | ConversationMember 实体（替代 chat_session_member.hxx）|
| `common/dao/mysql_conversation.hpp` | ConversationTable DAO |
| `common/dao/mysql_conversation_member.hpp` | ConversationMemberTable DAO |
| `conversation/source/conversation_server.h` | ConversationServiceImpl + ConversationServer + ConversationServerBuilder |
| `conversation/source/conversation_server.cc` | main 入口（gflags / etcd 注册 / 启动） |
| `conversation/CMakeLists.txt` | 编译 target `conversation_server` |
| `conf/conversation_server.conf` | gflags flagfile |

### 5.2 Builder 模式（仿 Relationship）

| 步骤 | 注入 |
|---|---|
| `make_mysql_object` | ODB database |
| `make_redis_object` | Redis（Members 缓存） |
| `make_es_object` | ESConversation client |
| `make_discovery_object` | identity / media / message 三个下游服务名 |
| `make_registry_object` | etcd `/service/conversation_service/...` |
| `make_rpc_object` | new ConversationServiceImpl |
| `build` + `start` | brpc server start |

`conversation/CMakeLists.txt` 的 `proto_files` 列表：

- `common/envelope.proto`、`common/types.proto`、`common/error.proto`
- `identity/identity_service.proto`
- `media/media_service.proto`
- `message/message_service.proto` ⚠️（编译会因 message_service.proto 缺 `option cc_generic_services` / 全限定不全阻塞，本期不修，等 Message 迁移期一起过；与 Relationship 当前同质）
- `conversation/conversation_service.proto`

`conf/conversation_server.conf` 由 `conf/chatsession_server.conf` 复制改名得到，gflag 名同步：`-message_service` 名空间维持，但内部值仍指向 message 服务发现 path（开发阶段不需要双名兼容）。

### 5.3 删除清单

| 路径 | 时机 |
|---|---|
| `chatsession/` 整目录 | Gateway 切换完 + 全仓 `grep ChatSessionService_Stub` 零命中后 |
| `proto/chatsession.proto` | 同上 |
| `conf/chatsession_server.conf` | 同上 |
| `odb/chat_session.hxx` / `chat_session_member.hxx` | rename 后即刻删 |
| `common/dao/mysql_chat_session*.hpp` | rename 后即刻删 |

### 5.4 修改清单（顶层）

| 路径 | 改动 |
|---|---|
| `proto/conversation/conversation_service.proto` | 删 36 处 user_id/session_id；加 `option cc_generic_services`；全限定引用 |
| 顶层 `CMakeLists.txt` | `add_subdirectory(chatsession)` → `add_subdirectory(conversation)` |
| `gateway/source/gateway_server.h` | 14 个 chatsession handler 切到新 stub + 新 RPC 名（详 §6） |
| `docker-compose.yml` | service 名 `chatsession_server` → `conversation_server`；image / depends_on 同步改；gateway / message / transmite 配置中 `--chatsession_service` → `--conversation_service` |

---

## 6. Gateway HTTP handler 切换（16 个：14 改 + 2 新增）

| HTTP 路由（保持） | 旧 stub / RPC | 新 stub / RPC |
|---|---|---|
| `/service/chatsession/get_session_list` | `ChatSessionService_Stub.GetChatSessionList` | `ConversationService_Stub.ListConversations` |
| `/service/chatsession/get_session_detail` | `GetChatSessionDetail` | `GetConversation` |
| `/service/chatsession/create` | `ChatSessionCreate` | `CreateConversation` |
| `/service/chatsession/set_name` | `SetChatSessionName` | `UpdateConversation`（仅 name） |
| `/service/chatsession/set_avatar` | `SetChatSessionAvatar` | `UpdateConversation`（仅 avatar_url=file_id） |
| `/service/chatsession/dismiss`（新）| — | `DismissConversation` |
| `/service/chatsession/add_member` | `AddChatSessionMember` | `AddMembers` |
| `/service/chatsession/remove_member` | `RemoveChatSessionMember` | `RemoveMembers` |
| `/service/chatsession/transfer_owner` | `TransferChatSessionOwner` | `TransferOwner` |
| `/service/chatsession/modify_role` | `ModifyMemberPermission` | `ChangeMemberRole` |
| `/service/chatsession/get_member` | `GetChatSessionMember` | `ListMembers` |
| `/service/chatsession/set_muted` | `SetSessionMuted` | `SetMute` |
| `/service/chatsession/set_pinned` | `SetSessionPinned` | `SetPin` |
| `/service/chatsession/set_visible` | `SetSessionVisible` | `SetVisible` |
| `/service/chatsession/quit` | `QuitChatSession` | `QuitConversation` |
| `/service/chatsession/mark_read` | `MsgReadAck` | `MarkRead` |
| `/service/chatsession/save_draft`（新）| — | `SaveDraft` |
| `/service/chatsession/search` | `SearchChatSession` | `SearchConversations` |

（注：旧 17 个 RPC 中 `SetChatSessionName` 和 `SetChatSessionAvatar` 合并到新 `UpdateConversation`，对应 2 个 HTTP 路由保留独立各自调 `UpdateConversation` 但只填一个 optional 字段。HTTP 路径前缀 `/service/chatsession/*` 暂不改名。）

每个 handler：

- `apply_auth_to_brpc(request, cntl, _auth)` 写 metadata（与 Relationship 切换的 6 个 friend handler 同款）
- 错误响应改写 `header.error_code` + `header.error_message`，不再用 `set_success / set_errmsg`

---

## 7. 验收标准

每一条都需在实施 plan 里展开成可测 step：

1. `proto/conversation/conversation_service.proto` 全文不含 `optional string user_id` / `optional string session_id`（grep 确认）；含 `option cc_generic_services = true`；引用全限定
2. `conversation_server` **代码完成**（编译验证留给 Message 迁移期统一过；与 Relationship 当前阻塞同质）
3. 18 个 RPC handler 全部使用 `HANDLE_RPC` 宏；从 metadata 取 user_id；不手填 ResponseHeader
4. `DismissConversation` 把 `conversation.status=DISMISSED`；后续 ListConversations 不再返回该会话
5. `SaveDraft` 写入 `conversation_member._draft`，下一次 ListConversations 在 SelfMemberInfo.draft 返回
6. `MarkRead` 仅在 `new_seq > old` 时更新 `last_read_seq`
7. `TransferOwner` 后旧 OWNER 降为 ADMIN；新 OWNER 必须先是成员
8. `CreateConversation(PRIVATE)` 同两个 user 重复调用返回相同 conversation_id（幂等）
9. Gateway 14 个 chatsession handler 全部切到 `ConversationService_Stub`，无 `ChatSessionService_Stub` 残留
10. 旧 `chatsession/` 目录、`proto/chatsession.proto`、`odb/chat_session*.hxx`、`mysql_chat_session*.hpp` 全部删除；`build/` 重新生成不出现 `chatsession_server` target

---

## 8. 兼容性

- **客户端 HTTP 路径不变**：沿用 `/service/chatsession/*`，与 Relationship 保留 `/service/friend/*` 同策略
- **proto 字段删除**：客户端继续传 `user_id` 字段无影响（proto3 unknown field 自动丢弃）
- **DB 表变更**：开发阶段 `docker-compose down -v` 重建。生产暂未上线，无 ALTER TABLE 风险
- **Message 服务依赖**：`SyncMessages` stub 是新 stub 名，编译阻塞与 Relationship 当前同质，等 Message 迁移期一起过

---

## 9. 工作量估算

| 模块 | 行数估 |
|---|---|
| ODB Conversation / ConversationMember rename + 字段确认 | ~50 |
| DAO ConversationTable / ConversationMemberTable + 4 新方法 | ~120 |
| ConversationServiceImpl 18 个 handler | ~700 |
| Server / Builder / main | ~180 |
| Gateway 14 handler 替换 + 2 新增 handler（dismiss / save_draft） | ~250 |
| proto 改动 + CMakeLists 联动 | ~50 |
| **合计** | **~1350** |

---

## 10. 后续工作（不在本 spec 范围）

- Message 服务迁移（最复杂，15 RPC + MQ 消费 + Outbox Reaper），完成后整体编译验证 + 集成验收
- Gateway HTTP 路径从 `/service/chatsession/*` → `/service/conversation/*`（与 Relationship `/service/friend/*` 一起在 Gateway 统一切换 spec 中处理）
- ES 索引 `chat_session` → `conversation` 重建（生产前最后一次性切换）
- unread_count 实时计数优化（Redis HyperLogLog 或 maintained counter，看产品 P95 列表延迟需求）
- GROUP `READ_RECEIPT_NOTIFY` 推送（YAGNI，看产品决策）

---

## 11. 实施记录（待填）

> **给下一位接手 Agent 的速读区。** 本节由实施 plan 执行过程中持续更新，记录代码已完成到什么程度、什么没验证、什么阻塞了哪一步、哪些约束必须遵守。

### 11.1 状态总览

| 类别 | 状态 |
|---|---|
| 全部 18 个 RPC 代码 | TBD |
| ODB Conversation / ConversationMember rename | TBD |
| DAO 4 个新方法 | TBD |
| Gateway 16 个 handler（14 改 + 2 新增）+ 服务名 rename | TBD |
| 旧 `chatsession/` 目录 + `proto/chatsession.proto` | TBD |
| docker-compose | TBD |
| **`conversation_server` 编译验证** | 预计阻塞（Message 未迁） |
| **集成验收（启动 + curl）** | 预计阻塞（依赖编译） |

### 11.2 commit 序列（按时间序，待填）

### 11.3 横切 hotfix（若有）

### 11.4 关键设计选择（执行时固化的）

### 11.5 ⚠️ 未完成项 / 已知阻塞

### 11.6 给下一位 Agent 的接手清单

### 11.7 结构性约束
