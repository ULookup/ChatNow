# ChatNow 服务层迁移设计

> 从旧 proto（flat 命名空间）到新 proto（domain 命名空间）的完整服务迁移设计。
> 本设计基于实际 proto 文件和 Spec（proto-redesign.md），所有 RPC 名称和类型均与最终 proto 一致。

---

## 一、设计决策速览（来自 Spec）

| 决策 | 说明 |
|---|---|
| Proto 独立于部署 | proto 文件边界不绑定进程边界，如 Presence proto 独立但和 Push 同进程 |
| Register/Login 用 oneof | `oneof credential { UsernamePassword, PhoneVerifyCode }` 合并用户名密码和手机验证码两套流程 |
| UpdateProfile 合并 | 一个 RPC 替代 SetAvatar/Nickname/Description/MailNumber，用 optional 字段区分 |
| MailRegister/MailLogin 废弃 | 不再支持独立邮箱注册登录，统一用用户名/手机 |
| 会话命名统一 Conversation | 所有 Session → Conversation，包括 ChatSessionService → ConversationService |
| sender_id 替代 sender UserInfo | 消息只传 sender_id，客户端本地缓存解析 |
| 无批量文件 RPC | Media 只有单数 UploadFile/DownloadFile（非复数） |
| 游标分页 | 大部分列表用 PageRequest/PageResponse，消息用 before_seq/after_seq + limit |
| GetMultiUserInfo 保留原名 | 未改名（不是 BatchGetProfiles），仍是 GetMultiUserInfo |

---

## 二、服务映射速查

### 2.1 服务名与 Proto 包

| 旧服务类 | 新 proto 包 | 新服务类 | 新 proto 文件 | 部署形态 |
|---|---|---|---|---|
| `UserService` | `chatnow.identity` | `IdentityService` | `identity/identity_service.proto` | 独立进程 |
| `FriendService` | `chatnow.relationship` | `RelationshipService` | `relationship/relationship_service.proto` | 独立进程 |
| `ChatSessionService` | `chatnow.conversation` | `ConversationService` | `conversation/conversation_service.proto` | 独立进程 |
| `MsgStorageService` | `chatnow.message` | `MessageService` | `message/message_service.proto` | 独立进程 |
| `FileService` | `chatnow.media` | `MediaService` | `media/media_service.proto` | 独立进程（与 Speech 共享 proto） |
| `SpeechService` | `chatnow.media` | `MediaService` | `media/media_service.proto` | 独立进程（与 File 共享 proto） |
| `PushService` | `chatnow.push` | `PushService` | `push/push_service.proto` | **独立进程，内含 Presence 模块** |
| `MsgTransmitService` | `chatnow.transmite` | `MsgTransmitService` | `transmite/transmite_service.proto` | 独立进程 |
| _(新)_ | `chatnow.presence` | `PresenceService` | `presence/presence_service.proto` | **Push 进程内模块，不独立部署** |

### 2.2 Stub 类名映射（跨服务调用）

```
UserService_Stub          → IdentityService_Stub
FriendService_Stub        → RelationshipService_Stub
ChatSessionService_Stub   → ConversationService_Stub
MsgStorageService_Stub     → MessageService_Stub
FileService_Stub          → MediaService_Stub
SpeechService_Stub        → MediaService_Stub
```

### 2.3 响应模式变化

**旧**：`response->set_success(false); response->set_errmsg("...");`
**新**：`response->mutable_header()->set_success(false); response->mutable_header()->set_error_code(...); response->mutable_header()->set_error_message("...");`

所有 `rsp.success()` 改为 `rsp.header().success()`，`rsp.errmsg()` 改为 `rsp.header().error_message()`。

---

## 三、逐服务迁移详情

### 3.1 Identity 服务（原 user/source/user_server.h，~799 行）

**继承**：`UserServiceImpl : public UserService` → `UserServiceImpl : public chatnow::IdentityService`

**Proto**：`identity/identity_service.proto`（9 个 RPC）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| MySQL (`UserTable`) | ODB DAO | 用户 CRUD |
| ElasticSearch (`ESUser`) | ES client | 用户搜索 |
| Redis (`Session`, `Status`, `Codes`) | Redis | 登录会话、在线状态、验证码 |
| MailClient | SMTP | 发送手机/邮箱验证码 |
| MediaService (RPC) | 服务依赖 | 头像文件上传/下载 |
| etcd (`Registry`) | 服务发现 | 注册 |

**RPC 迁移表**：

| 旧 RPC | 新 RPC | 关键变化 |
|---|---|---|
| `UserRegister` | `Register` | `oneof credential { UsernamePassword, PhoneVerifyCode }` 合并用户名+手机两种注册方式 |
| `UserLogin` | `Login` | 同上，`oneof credential` 合并登录方式 |
| _(无)_ | `Logout` | **新增**：登出 |
| `GetMailVerifyCode` | `SendVerifyCode` | 改为发送手机验证码 |
| _(无)_ | `RefreshToken` | **新增**：刷新 session |
| `GetUserInfo` | `GetProfile` | 单个用户信息查询 |
| `SetUserAvatar` + `SetUserNickname` + `SetUserDescription` + `SetUserMailNumber` | `UpdateProfile` | **四个合并为一个**，通过 optional 字段（nickname/bio/avatar_url/phone）区分 |
| `GetMultiUserInfo` | `GetMultiUserInfo` | **保留原名**，请求 `repeated string users_id`，响应 `map<string, UserInfo> users_info` |
| _(无)_ | `SearchUsers` | **新增**：用户搜索（从旧 FriendService.FriendSearch 拆分） |

**废弃的旧 RPC**：`MailRegister`、`MailLogin`（邮箱注册登录不再支持）

**跨服务调用**：
- `FileService_Stub.GetSingleFile/PutSingleFile` → `MediaService_Stub.DownloadFile/UploadFile`

**迁移要点**：
1. 继承类名改为 `IdentityService`
2. 9 个 RPC 签名替换（含 3 个新增：Logout、RefreshToken、SearchUsers）
3. `SetUserAvatar/Nickname/Description/MailNumber` 四个旧 RPC 合并为一个 `UpdateProfile`
4. 响应模式改为 `ResponseHeader`
5. `FileService_Stub` → `MediaService_Stub`
6. `GetMultiUserInfo` 保留原名，但类型引用路径从旧 proto 切换到新 proto 的 `chatnow.identity.GetMultiUserInfoReq/Rsp`

---

### 3.2 Relationship 服务（原 friend/source/friend_server.h，~683 行）

**继承**：`FriendServiceImpl : public FriendService` → `FriendServiceImpl : public chatnow::RelationshipService`

**Proto**：`relationship/relationship_service.proto`（9 个 RPC）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| MySQL (`FriendApplyTable`, `RelationTable`) | ODB DAO | 好友申请、好友关系 |
| MySQL (`ChatSessionTable`, `ChatSessionMemberTable`) | ODB DAO | 好友通过后创建单聊会话（待改为调用 ConversationService） |
| ElasticSearch (`ESUser`) | ES client | 好友搜索 |
| IdentityService (RPC) | 服务依赖 | `GetMultiUserInfo` 获取好友用户信息 |
| etcd (`Discovery`, `Registry`) | 服务发现 | 注册/发现 |

**RPC 迁移表**：

| 旧 RPC | 新 RPC | 关键变化 |
|---|---|---|
| `GetFriendList` | `ListFriends` | 分页改用 `PageRequest/PageResponse` |
| `FriendAdd` | `SendFriendRequest` | |
| `FriendAddProcess` | `HandleFriendRequest` | 用 `bool agree` 区分同意/拒绝，非拆成两个 RPC |
| `FriendRemove` | `RemoveFriend` | |
| `FriendSearch` | `SearchFriends` | 注意：`SearchUsers` 在 Identity 上，此为 SearchFriends |
| `GetPendingFriendEventList` | `ListPendingRequests` | 响应 `repeated FriendEvent event` |
| _(无)_ | `BlockUser` | **新增** |
| _(无)_ | `UnblockUser` | **新增** |
| _(无)_ | `ListBlockedUsers` | **新增** |

**跨服务调用**：
- `UserService_Stub.GetMultiUserInfo` → `IdentityService_Stub.GetMultiUserInfo`（注意：方法名没变，但 stub 类变了）

**迁移要点**：
1. 继承类名改为 `RelationshipService`
2. 6 个旧 RPC → 6 个新 RPC 签名替换 + 3 个新增（Block/Unblock/ListBlocked）
3. `HandleFriendRequest` 通过 `bool agree` 替代旧的 `FriendAddProcess`
4. GetMultiUserInfo stub 类名更新
5. 响应模式改为 `ResponseHeader`
6. 好友通过后创建会话逻辑：可考虑改为调用 `ConversationService.CreateConversation`（替代直接操作 ChatSessionTable）

---

### 3.3 Conversation 服务（原 chatsession/source/chatsession_server.h，~1210 行）

**继承**：`ChatSessionServiceImpl : public ChatSessionService` → `ChatSessionServiceImpl : public chatnow::ConversationService`

**Proto**：`conversation/conversation_service.proto`（18 个 RPC）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| MySQL (`ChatSessionTable`, `ChatSessionMemberTable`) | ODB DAO | 会话/成员 CRUD |
| ElasticSearch (`ESChatSession`) | ES client | 会话搜索 |
| Redis (`Members`) | Redis client | 成员缓存 |
| IdentityService (RPC) | 服务依赖 | `GetMultiUserInfo` 获取成员用户信息 |
| MediaService (RPC) | 服务依赖 | `UploadFile` 上传群头像 |
| MessageService (RPC) | 服务依赖 | 最近消息同步 |

**RPC 迁移表**：

| 旧 RPC | 新 RPC | 额外说明 |
|---|---|---|
| `GetChatSessionList` | `ListConversations` | 分页用 PageRequest/PageResponse，响应含 Conversation 含 SelfMemberInfo |
| `GetChatSessionDetail` | `GetConversation` | 返回单个 Conversation |
| `ChatSessionCreate` | `CreateConversation` | |
| `SetChatSessionName` + `SetChatSessionAvatar` | `UpdateConversation` | **合并**：通过 optional name/avatar_url/description 区分 |
| _(无)_ | `DismissConversation` | **新增**：解散群 |
| `AddChatSessionMember` | `AddMembers` | |
| `RemoveChatSessionMember` | `RemoveMembers` | |
| `TransferChatSessionOwner` | `TransferOwner` | |
| `ModifyMemberPermission` | `ChangeMemberRole` | MemberRole 枚举：MEMBER/ADMIN/OWNER |
| `GetChatSessionMember` | `ListMembers` | 分页用 PageRequest/PageResponse，返回 MemberItem 含 UserInfo+MemberRole |
| `SetSessionMuted` | `SetMute` | 响应 SelfMemberInfo |
| `SetSessionPinned` | `SetPin` | 响应 SelfMemberInfo |
| `SetSessionVisible` | `SetVisible` | 响应 SelfMemberInfo |
| `QuitChatSession` | `QuitConversation` | |
| `MsgReadAck` | `MarkRead` | 字段 `seq_id`（非 message_id） |
| _(无)_ | `SaveDraft` | **新增**：草稿保存 |
| `SearchChatSession` | `SearchConversations` | |
| `GetMemberIdList` | `GetMemberIds` | 内部接口（Transmite 依赖），保留 |

**跨服务调用**：
- `UserService_Stub.GetMultiUserInfo` → `IdentityService_Stub.GetMultiUserInfo`
- `FileService_Stub.PutSingleFile` → `MediaService_Stub.UploadFile`
- `MsgStorageService_Stub.GetRecentMsg` → `MessageService_Stub.SyncMessages`

**迁移要点**：
1. 继承类名、17 个 RPC 签名全面替换
2. `UpdateConversation` 合并旧的两个 RPC
3. 新增 `DismissConversation`、`SaveDraft`
4. ServiceManager 中的服务名字符串更新（配置层面）
5. 3 个内部辅助函数的 stub 类名和 RPC 方法名更新
6. 响应模式改为 `ResponseHeader`
7. `MarkRead` 字段名从 message_id 改为 seq_id

---

### 3.4 Message 服务（原 message/source/message_server.h，~1387 行）

**这是最复杂的服务，包含 RPC + MQ 消费 + Outbox Reaper。**

**继承**：`MessageServiceImpl : public MsgStorageService` → `MessageServiceImpl : public chatnow::MessageService`

**Proto**：`message/message_service.proto`（**15 个 RPC**）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| MySQL (`MessageTable`) | ODB DAO | 消息主表 |
| MySQL (`UserTimelineTable`) | ODB DAO | 用户时间线 |
| MySQL (`ChatSessionMemberTable`) | ODB DAO | 成员 last_ack_seq |
| MySQL (`MessageReaction`, `MessagePin`) | ODB DAO | **新增**：reactions、pin 表 |
| ElasticSearch (`elasticlient::Client`) | ES client | 消息搜索索引 |
| RabbitMQ (`Subscriber` ×2, `Publisher` ×2) | MQ | 消费 DB 消息 + ES 事件；发布 Push + ES 索引 |
| Redis (`SeqGen`, `PushOutbox`, `ESOutbox`) | Redis | seq 生成、未送达队列、ES 索引队列 |
| IdentityService (RPC) | 服务依赖 | `GetMultiUserInfo` 填充发送者信息 |
| MediaService (RPC) | 服务依赖 | `DownloadFile` 填充文件信息 |

**RPC 迁移表（15 个）**：

| 旧 RPC | 新 RPC | 关键变化 |
|---|---|---|
| `GetHistoryMsg` | `GetHistory` | 分页用 `before_seq + limit`，非 PageRequest |
| `GetRecentMsg` | `SyncMessages` | 分页用 `after_seq + limit`，合并旧 GetOfflineMsg 功能 |
| `MsgSearch` | `SearchMessages` | cursor 游标分页 + keyword |
| `GetOfflineMsg` | _(合并入 SyncMessages)_ | 通过 after_seq=0 实现离线拉取 |
| `GetUnreadCount` | _(合并入 Conversation.SelfMemberInfo)_ | 未读数从会话列表返回，不单独 RPC |
| `SelectByClientMsg` | `SelectByClientMsgId` | 幂等去重 |
| `UpdateAckSeq` | `UpdateReadAck` | 字段名 message_id→seq_id |
| _(无)_ | `GetMessagesById` | **新增**：按 message_id 批量查 |
| _(无)_ | `RecallMessage` | **新增**：撤回消息 |
| _(无)_ | `AddReaction` | **新增**：添加 reaction |
| _(无)_ | `RemoveReaction` | **新增**：移除 reaction |
| _(无)_ | `GetReactions` | **新增**：查询消息 reactions |
| _(无)_ | `PinMessage` | **新增**：置顶消息 |
| _(无)_ | `UnpinMessage` | **新增**：取消置顶 |
| _(无)_ | `ListPinnedMessages` | **新增**：列出置顶消息 |
| _(无)_ | `DeleteMessages` | **新增**：批量删除消息 |
| _(无)_ | `ClearConversation` | **新增**：清空会话消息 |

**MQ 消费链路**：

- **onDBMessage**（消费 transmite → MQ）：
  - 反序列化 `chatnow::InternalMessage`（旧） → `chatnow::message::internal::InternalMessage`（新，注意 `.internal` 子包）
  - 写 Message 主表 + UserTimeline + 索引 MessageReaction / MessagePin 表
  - 发布 ESIndexEvent 到 ES MQ
  - 发布 Push 通知到 Push MQ

- **onESIndexMessage**（消费 ES 索引事件）：
  - 反序列化 `chatnow::ESIndexEvent` → `chatnow::message::internal::ESIndexEvent`

**跨服务调用**：
- `UserService_Stub.GetMultiUserInfo` → `IdentityService_Stub.GetMultiUserInfo`
- `FileService_Stub.GetMultiFile` → `MediaService_Stub.DownloadFile`（循环调用单数）
- `FileService_Stub.PutSingleFile` → `MediaService_Stub.UploadFile`

**迁移要点**：
1. 继承类名、15 个 RPC 签名替换（含 10 个新增）
2. 分页从自定义 offset/limit 改为 before_seq/after_seq + limit
3. `InternalMessage` proto 引用路径更新：旧 proto → `message/message_internal.pb.h`（`chatnow.message.internal` 包）
4. `MessageType` 枚举值替换（`SPEECH→AUDIO`, `CARD→STICKER` 等）
5. 3 个内部辅助函数 stub 更新
6. 响应模式全面切换
7. 新增 ODB 表 MessageReaction / MessagePin 的 DAO 操作
8. `GetUnreadCount` 逻辑迁移到 Conversation 服务

---

### 3.5 Transmite 服务（原 transmite/source/transmite_server.h，~545 行）

**Proto**：`transmite/transmite_service.proto`（1 个 RPC）

**继承**：`TransmiteServiceImpl : public chatnow::MsgTransmitService`（类名不变，但 proto 类型变了）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| RabbitMQ (`Publisher`) | MQ | 发布 InternalMessage |
| Redis (`SeqGen`, `Members`, `RateLimiter`) | Redis | seq 生成、成员缓存、限流 |
| Snowflake (`SnowflakeId`) | 本地 | 分布式 message_id |
| WorkerIdAllocator | Redis | Snowflake worker 租约 |
| IdentityService (RPC) | 服务依赖 | `GetProfile` 获取发送者信息 |
| ConversationService (RPC) | 服务依赖 | `GetMemberIds` 获取会话成员 |
| MessageService (RPC) | 服务依赖 | `SelectByClientMsgId` 幂等去重 |

**RPC 迁移**：

| 旧 | 新 | 变化 |
|---|---|---|
| `GetTransmitTarget(NewMessageReq)` | `GetTransmitTarget(NewMessageReq) → GetTransmitTargetRsp` | 请求类型基本不变，响应增加 `ResponseHeader` |

**跨服务调用**：
- `UserService_Stub.GetUserInfo` → `IdentityService_Stub.GetProfile`
- `ChatSessionService_Stub.GetMemberIdList` → `ConversationService_Stub.GetMemberIds`
- `MsgStorageService_Stub.SelectByClientMsg` → `MessageService_Stub.SelectByClientMsgId`

**迁移要点**：
1. 3 个 stub 调用更新
2. 读取下游响应时 `rsp.success()` → `rsp.header().success()`
3. 构造 `InternalMessage` 时切换到 `chatnow.message.internal` 包
4. 通知类型更新为新的 `NotifyType` 枚举值

---

### 3.6 Push 服务 + Presence 模块（原 push/source/push_server.h，~933 行）

**部署形态**：Push 独立进程，内含 Presence 模块（proto 独立但运行在同进程）。

**Push 部分继承**：`PushServiceImpl : public PushService` → `PushServiceImpl : public chatnow::PushService`
**Presence 部分继承**：实现 `chatnow::PresenceService`（`presence/presence_service.proto`，6 个 RPC），在同一进程注册。

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| WebSocket (`Connection`) | 内存 | uid → conn 映射 |
| Redis (`Session`, `Status`, `OnlineRoute`, `UnackedPush`, `CrossInstanceOutbox`) | Redis | 在线路由、未 ack 缓冲、跨实例投递 |
| Redis (`Presence`, `PresenceDevices`, `PresenceTyping`, `PresenceSub`) | Redis | **Presence 模块**：状态 HASH、设备 SET、typing SET、订阅 SET |
| RabbitMQ (`Subscriber`) | MQ | 消费推送队列 |
| MessageService (RPC) | 服务依赖 | `UpdateReadAck`、`SyncMessages` |
| PushService 自身 (RPC) | 服务依赖 | 跨实例 `PushBatch` |

**Push RPC**（不变）：

| RPC | 变化 |
|---|---|
| `PushToUser` | 响应增加 `ResponseHeader`，`UserSeqPair` 类型在 `chatnow.push` 包 |
| `PushBatch` | 同上 |

**Presence RPC（6 个，在 Push 进程内实现）**：

| RPC | 说明 |
|---|---|
| `SetPresence` | 用户上线/离线/隐身/AWAY/BUSY |
| `GetPresence` | 查单个用户状态 |
| `BatchGetPresence` | 批量查用户状态 |
| `SubscribePresence` | 订阅好友状态变更 |
| `UnsubscribePresence` | 取消订阅 |
| `SendTyping` | 输入中通知 |

注意：Presence 无 `Heartbeat` RPC。Push 收到 WS 心跳后直接本地操作 Redis Presence key，不通过 RPC。

**Push 内部处理变化**：

- `onPushMessage`（MQ 消费）：
  - 反序列化 `InternalMessage` → `chatnow.message.internal.InternalMessage`
  - `NotifyType` 枚举：`CHAT_MESSAGE_NOTIFY`、`MESSAGE_RECALLED_NOTIFY`、`PRESENCE_CHANGE_NOTIFY`、`TYPING_NOTIFY` 等（无 `NOTIFY_TYPE_` 前缀）
  - `NotifyMessage.oneof notify_remarks` 包含所有通知子类型

- `onClientNotify`（WS 消息处理）：
  - 鉴权：`NotifyClientAuth`（session_id, device_id）
  - ACK：`NotifyMsgPushAck`（user_id, message_id, user_seq, conversation_id）
  - 心跳：`NotifyHeartbeat`（user_id, last_user_seq）
  - 心跳处理中直接更新 Presence 的 Redis key（无 RPC 开销）

- `_on_heartbeat_resend` → 调用 `MessageService_Stub.SyncMessages`

**跨服务调用**：
- `MsgStorageService_Stub.UpdateAckSeq` → `MessageService_Stub.UpdateReadAck`
- `MsgStorageService_Stub.GetOfflineMsg` → `MessageService_Stub.SyncMessages`

**注意：UserSeqPair 类型冲突**
`UserSeqPair` 在 `push/push_service.proto`（`chatnow.push.UserSeqPair`）和 `message/message_internal.proto`（`chatnow.message.internal.UserSeqPair`）中重复定义。跨服务传递时需确保使用同一类型。

**迁移要点**：
1. Push RPC 签名基本不变，响应改为 ResponseHeader
2. 实现 PresenceService 6 个 RPC（在 Push 进程的同一个 server 上 `AddService`）
3. `onPushMessage` 中 `InternalMessage` 反序列化路径改为 `chatnow.message.internal`
4. `NotifyType` 枚举值更新（`CHAT_MESSAGE_NOTIFY` 等）
5. `onClientNotify` 中的字段读取更新（`NotifyClientAuth` 等新类型）
6. Heartbeat 处理中直接更新 Presence Redis key
7. CrossInstanceOutbox reaper 中 InternalMessage 更新

---

### 3.7 Media 服务（原 file/source/file_server.h + speech/source/speech_server.h，~183+145=328 行）

**Proto**：`media/media_service.proto`（4 个 RPC），proto 层合并但进程层保持分离。

**两类 binary**：
- **Media (File)** — FileServiceImpl 实现 `chatnow::MediaService`，注册 3 个 RPC（UploadFile, DownloadFile, GetFileInfo）
- **Media (Speech)** — SpeechServiceImpl 实现 `chatnow::MediaService`，注册 1 个 RPC（SpeechRecognition）

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| 本地文件系统 | 磁盘 | File 部分：文件存储读写 |
| ASRClient | 语音 SDK | Speech 部分：语音识别 |

**RPC 迁移表**：

| 旧 RPC | 新 RPC | 关键变化 |
|---|---|---|
| `PutSingleFile` | `UploadFile` | 单数，非复数。请求含 `FileUploadData { file_name, file_size, file_content }` |
| `GetSingleFile` | `DownloadFile` | 单数 |
| `PutMultiFile` | _(无对应)_ | **已移除**。调用方改为循环调 UploadFile |
| `GetMultiFile` | _(无对应)_ | **已移除**。调用方改为循环调 DownloadFile |
| _(无)_ | `GetFileInfo` | **新增**：查文件元信息 |
| `SpeechRecognition` | `SpeechRecognition` | **保留原名** |

**被多少服务依赖**：
- UploadFile/DownloadFile 被 Message、Identity、Conversation 三个服务调用
- SpeechRecognition 通过 Gateway 被客户端调用

**迁移要点**：
1. File: 4 个旧 RPC → 3 个新 RPC（UploadFile, DownloadFile, GetFileInfo），响应改为 ResponseHeader
2. Speech: 1 个 RPC 改名（保留），响应改为 ResponseHeader
3. 调用方（Message/Conversation）原来调 `GetMultiFile`/`PutMultiFile` 的逻辑需改为循环调单数接口
4. 改动量最小（~100 行）

---

### 3.8 Gateway 服务（原 gateway/source/gateway_server.h，~1500+ 行）

**Gateway 不继承 proto Service 类**，是 HTTP → RPC 协议转换层。

**中间件依赖**：

| 依赖 | 类型 | 用途 |
|---|---|---|
| httplib | HTTP server | 接收客户端 HTTP 请求 |
| Redis (`Session`) | Redis | session_id → uid 鉴权 |
| etcd (`Discovery`, `ServiceManager`) | 服务发现 | 后端路由 |

**影响范围：~34 个 HTTP handler，全部需要更新**。

**HTTP 路由 → RPC 映射（按服务分组）**：

**Identity（~11 handlers）**：

| HTTP 路由 | 旧调用 | 新调用 |
|---|---|---|
| `USERNAME_REGISTER` | `UserService_Stub.UserRegister` | `IdentityService_Stub.Register` |
| `USERNAME_LOGIN` | `UserService_Stub.UserLogin` | `IdentityService_Stub.Login` |
| _(无)_ | — | `IdentityService_Stub.Logout`（新增） |
| `GET_MAIL_VERIFY_CODE` | `UserService_Stub.GetMailVerifyCode` | `IdentityService_Stub.SendVerifyCode` |
| `MAIL_REGISTER` | `UserService_Stub.MailRegister` | **废弃** |
| `MAIL_LOGIN` | `UserService_Stub.MailLogin` | **废弃** |
| `GET_USER_INFO` | `UserService_Stub.GetUserInfo` | `IdentityService_Stub.GetProfile` |
| `SET_AVATAR` | `UserService_Stub.SetUserAvatar` | `IdentityService_Stub.UpdateProfile`（optional avatar_url） |
| `SET_NICKNAME` | `UserService_Stub.SetUserNickname` | `IdentityService_Stub.UpdateProfile`（optional nickname） |
| `SET_DESCRIPTION` | `UserService_Stub.SetUserDescription` | `IdentityService_Stub.UpdateProfile`（optional bio） |
| `SET_MAIL` | `UserService_Stub.SetUserMailNumber` | `IdentityService_Stub.UpdateProfile`（optional phone） |
| `GET_MULTI_USER_INFO` | `UserService_Stub.GetMultiUserInfo` | `IdentityService_Stub.GetMultiUserInfo` |

**Relationship（~6 handlers）**：

| HTTP 路由 | 旧调用 | 新调用 |
|---|---|---|
| `GET_FRIEND_LIST` | `FriendService_Stub.GetFriendList` | `RelationshipService_Stub.ListFriends` |
| `ADD_FRIEND_APPLY` | `FriendService_Stub.FriendAdd` | `RelationshipService_Stub.SendFriendRequest` |
| `ADD_FRIEND_PROCESS` | `FriendService_Stub.FriendAddProcess` | `RelationshipService_Stub.HandleFriendRequest` |
| `REMOVE_FRIEND` | `FriendService_Stub.FriendRemove` | `RelationshipService_Stub.RemoveFriend` |
| `SEARCH_FRIEND` | `FriendService_Stub.FriendSearch` | `RelationshipService_Stub.SearchFriends` |
| `GET_PENDING_FRIEND_EVENTS` | `FriendService_Stub.GetPendingFriendEventList` | `RelationshipService_Stub.ListPendingRequests` |
| _(无)_ | — | `RelationshipService_Stub.BlockUser` / `UnblockUser` / `ListBlockedUsers`（新增） |

**Conversation（~14 handlers）**：所有 `ChatSessionService_Stub` → `ConversationService_Stub`，RPC 方法名从 Session 改为 Conversation 命名。

**Message（~5 handlers）**：

| HTTP 路由 | 旧调用 | 新调用 |
|---|---|---|
| `GET_HISTORY` | `MsgStorageService_Stub.GetHistoryMsg` | `MessageService_Stub.GetHistory` |
| `GET_RECENT` | `MsgStorageService_Stub.GetRecentMsg` | `MessageService_Stub.SyncMessages` |
| `SEARCH_HISTORY` | `MsgStorageService_Stub.MsgSearch` | `MessageService_Stub.SearchMessages` |
| `NEW_MESSAGE` | `MsgTransmitService_Stub.GetTransmitTarget` | 不变（但 Req/Rsp 类型已更新） |
| _(新增)_ | — | `MessageService_Stub.RecallMessage` 等 |

**Media（~5 handlers）**：

| HTTP 路由 | 旧调用 | 新调用 |
|---|---|---|
| `GET_SINGLE_FILE` | `FileService_Stub.GetSingleFile` | `MediaService_Stub.DownloadFile` |
| `GET_MULTI_FILE` | `FileService_Stub.GetMultiFile` | **废弃**（改为客户端循环调 DownloadFile） |
| `PUT_SINGLE_FILE` | `FileService_Stub.PutSingleFile` | `MediaService_Stub.UploadFile` |
| `PUT_MULTI_FILE` | `FileService_Stub.PutMultiFile` | **废弃** |
| `RECOGNITION` | `SpeechService_Stub.SpeechRecognition` | `MediaService_Stub.SpeechRecognition` |

**Gateway 特有要点**：
- 后端响应 `response.set_content(rsp.SerializeAsString(), ...)` 不需要改动
- 错误检查：`rsp.success()` → `rsp.header().success()`，`rsp.errmsg()` → `rsp.header().error_message()`
- `_pushNotify` 中 `NotifyMessage` 组装逻辑对齐新 `chatnow.push.NotifyMessage`（oneof notify_remarks）
- `_GetUserInfo` 辅助函数：`UserService_Stub.GetUserInfo` → `IdentityService_Stub.GetProfile`
- 服务名字符串需配置文件同步更新

---

## 四、跨服务调用矩阵

| 调用方 ↓ / 被调用方 → | Identity | Relationship | Conversation | Message | Media | Push | Transmite |
|---|---|---|---|---|---|---|---|
| **Gateway** | 11 RPC | 9 RPC | 17 RPC | 5+ RPC | 4 RPC | PushToUser | 1 RPC |
| **Message** | GetMultiUserInfo | — | — | — | DownloadFile | — | — |
| **Conversation** | GetMultiUserInfo | — | — | SyncMessages | UploadFile | — | — |
| **Relationship** | GetMultiUserInfo | — | — | — | — | — | — |
| **Transmite** | GetProfile | — | GetMemberIds | SelectByClientMsgId | — | — | — |
| **Push** | — | — | — | UpdateReadAck, SyncMessages | — | PushBatch | — |

**连锁影响关键路径**：
- `GetMultiUserInfo`：被 4 个服务调用（保留原名，只改 stub 类名，改动最小）
- `SyncMessages`：被 Conversation 和 Push 调用
- `DownloadFile` / `UploadFile`：被 Message、Conversation、Identity 调用（注意无批量接口）

---

## 五、Presence 模块特殊说明

Presence 不在跨服务调用矩阵中（不通过 RPC 被其他服务调用），原因是：

1. **Presence 运行在 Push 进程内**，与 Push 共享 Redis 连接、WS 连接
2. 客户端通过 WS（Push 长连接）直接订阅 Presence 变更
3. 心跳在 Push 内处理，直接写 Redis Presence key（无 RPC 开销）
4. `BatchGetPresence` 可能被 Conversation 服务调用来显示成员在线状态，但这是可选依赖

如果后续需要跨服务访问 Presence，通过 Push 进程上的 `PresenceService` 调用即可（与 Push RPC 使用同一个 brpc server）。

---

## 六、建议执行顺序

| 顺序 | 服务 | 理由 |
|---|---|---|
| 1 | **Media (File + Speech)** | 依赖最少，改动最小（~100 行），验证迁移模式 |
| 2 | **Identity** | 只依赖 Media，GetMultiUserInfo 是下游关键接口，先迁好 |
| 3 | **Relationship** | 依赖 Identity，含 Block/Unblock 新增功能 |
| 4 | **Conversation** | 依赖 Identity + Media + Message，Message 的 stub 名先改为新的，Message 本身可后迁 |
| 5 | **Message** | 最复杂（15 RPCs），依赖 Identity + Media |
| 6 | **Transmite** | 依赖 Identity + Conversation + Message |
| 7 | **Push + Presence** | Push 依赖 Message，Presence 模块在 Push 内实现 |
| 8 | **Gateway** | 最后迁，一次性切换所有 34 个 HTTP handler |

---

## 七、工作量估算（修正后）

| 服务 | RPC 数 | 新增 RPC | 跨服务调用数 | 预估改动行数 | 难度 |
|---|---|---|---|---|---|
| Media (File + Speech) | 4 | 1 | 0 | ~100 | 低 |
| Identity | 9 | 3 | 1 | ~450 | 中 |
| Relationship | 9 | 3 | 1 | ~400 | 中 |
| Conversation | 17 | 2 | 3 | ~700 | 高 |
| Message | 15 | 10 | 2 | ~1500 | 最高 |
| Transmite | 1 | 0 | 3 | ~300 | 中 |
| Push + Presence | 2+6 | 6 | 2 | ~800 | 高 |
| Gateway | 0 (34 HTTP) | ~5 | 7 | ~1000 | 高 |
| **总计** | **64** | **30** | — | **~5250** | — |
