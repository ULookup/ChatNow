# ChatNow Proto 设计与主流 IM 差距清单

> **状态**: 评审记录（仅记录差距，不含修订方案）
> **日期**: 2026-05-14
> **基线**: proto/ 当前 9 个域 proto 实际实现 + `2026-05-14-proto-redesign.md` 设计稿
> **参照**: 微信、Telegram (MTProto)、Discord、Slack、WhatsApp、Signal、飞书 IM IDL

本文记录评审中发现的、当前 proto 设计偏离主流 IM 架构的具体点。**不含修订方案**——下一步评审会决定哪些项纳入修订、哪些延后或不做。

---

## 一、必须修复（影响可上线性 / 工程基础）

### G1. 鉴权字段重复出现在每个业务 Req 里

**现状**：几乎所有业务 Req 体内重复以下两字段：
```
optional string user_id = 2;
optional string session_id = 3;
```
波及：`conversation_service.proto`（17 个 RPC 全有）、`relationship_service.proto`、`identity_service.proto`、`media_service.proto`、`transmite_service.proto`。`proto-redesign.md` 第 0 节自身已批评（"auth 字段重复 25+ 处"），但落地 proto 并未修正。

**主流做法**：鉴权走 transport metadata（gRPC metadata、HTTP `Authorization` header、brpc Controller attachment）。proto 业务体不携带 session_id/user_id。

**影响**：
- 鉴权机制变更（如引入 OAuth2、JWT、设备级 token）需改所有 Req 定义
- 服务实现里 auth 解析逻辑在每个 handler 重复
- 跨服务内部调用（如 Conversation → Identity）也要伪造 session_id，语义混乱

---

### G2. Media 文件传输使用内联 `bytes file_content`

**现状**：
```
// media_service.proto
FileUploadData { ..., bytes file_content = 3; }
FileDownloadData { ..., bytes file_content = 2; }
```

**主流做法**（微信、Telegram、Slack、Discord、飞书、WhatsApp 全部）：
- 上传：客户端 → MediaService 申请预签名 PUT URL → 直传对象存储（S3/OSS/MinIO） → 回调 `CompleteUpload(file_id)` 落库
- 下载：proto 返回 `download_url`（CDN 短期签名），客户端直接 GET

**影响**：
- 单消息 brpc 帧上限受限（默认 64MB），>10MB 文件即不可用
- 视频、原图、长语音根本无法上传
- CDN 不可用、断点续传不可用、文件去重（content hash）不可用
- Message/Conversation/Identity 三处调用方都要 base64-in-protobuf，内存放大严重

---

### G3. 多端在线 / Device 模型缺失

**现状**：
- `LoginReq` 含 `device_id` + `device_name`，但 proto 中没有 `Device` 实体
- 没有 `ListDevices / KickDevice / GetDeviceInfo` RPC
- `Presence.active_device_types = repeated string`（仅 "phone"/"pc" 字符串），非结构化
- Logout 只能登出当前 session，无法"踢另一台设备"

**主流做法**（WhatsApp Multi-Device、Signal Linked Devices、微信 PC/Mac、Telegram Active Sessions、飞书多端）：
- `Device { device_id, platform, model, app_version, last_seen_at, ip, location, is_current }`
- `ListDevices() / RevokeDevice(device_id) / RenameDevice`
- Presence 按设备维度区分（手机离线但 PC 在线 = ONLINE_PC）

**影响**：产品上线后无法支持"在另一台设备登录会顶掉/共存"、"远程退出"、"设备列表"等基础功能；Presence 信息粒度过粗。

---

## 二、建议修复（架构清晰性 / 性能 / 可演进性）

### G4. ListConversations 无增量同步 token

**现状**：
```
ListConversationsRsp {
    repeated Conversation conversations = 2;
    PageResponse page = 3;
}
```
仅支持游标分页全量拉取，无"增量同步"路径。`Conversation` 实体没有 `updated_at_ms` 或 `revision` 字段。

**主流做法**：
- Slack `conversations.list` 配合 `users.conversations` + `latest` 时间戳
- Telegram updates state（pts/qts/seq）
- 飞书 sync_token：服务端返回 token，客户端下次带回，仅返回 delta
- WhatsApp Multi-Device sync key

**影响**：冷启动有 100+ 会话场景下，每次重连都全量拉，浪费带宽和后端 ES/MySQL；客户端无法做"会话级增量更新"。

---

### G5. PushService 用 `bytes notify_payload` 弱类型透传

**现状**：
```
PushToUserReq { ..., bytes notify_payload = 3; }
PushBatchReq  { ..., bytes notify_payload = 3; }
```
调用方（Message/Transmite）需要先把 `NotifyMessage` 序列化为 bytes，Push 进程再反序列化推给 WS。

**主流做法**：内部 RPC 用强类型（`NotifyMessage notify = 3`），仅在 WS 边界做一次序列化。

**影响**：
- 多一次 (序列化 → 反序列化) 开销
- 类型安全弱，调用方改字段不会被下游编译期发现
- 排查问题时 brpc 日志看到的是 bytes，需要手动 dump

---

### G6. `NotifyType` 把业务通知和控制帧混在一个 enum

**现状**（`push/notify.proto`）：
```
enum NotifyType {
    FRIEND_ADD_APPLY_NOTIFY = 0;
    ...
    CHAT_MESSAGE_NOTIFY = 3;
    ...
    CLIENT_AUTH = 49;       // 控制帧
    MSG_PUSH_ACK = 50;      // 控制帧
    CLIENT_HEARTBEAT = 51;  // 控制帧
}
```
数值跳跃（3 → 49）说明设计者已隐约感觉到两类不应共存。

**主流做法**（Discord Gateway 是教科书案例）：
- OP code（传输/控制层）：0=Dispatch, 1=Heartbeat, 2=Identify, 9=InvalidSession, 10=Hello, 11=HeartbeatAck
- Event type（业务层）：MESSAGE_CREATE / TYPING_START / PRESENCE_UPDATE
- 两个 enum 完全独立

**影响**：客户端帧路由分发逻辑不优雅；新增控制帧（如 `RECONNECT`、`SERVER_SHUTDOWN_NOTICE`）需要污染业务 enum。

---

### G7. Reactions 无变更 notify 路径

**现状**：
- `Message.reactions = repeated ReactionGroup`（嵌入消息体）
- `MessageService.AddReaction / RemoveReaction` 写入数据库
- `notify.proto` 中**无** `NotifyReactionChange`

**问题**：用户 A 给消息加 reaction，用户 B 在同一会话中如何感知？目前路径：B 必须重新拉取整条 Message。

**主流做法**：reaction 是高频独立事件，单独的 push notify（`NotifyReactionAdded { conversation_id, message_id, emoji, user_id, total_count }`），客户端本地 patch 已显示的消息。

**影响**：reactions 在群聊中不可用（无实时性）；或者客户端必须轮询 `GetReactions`，浪费带宽。

---

### G8. CreateConversationReq 缺少 type 字段

**现状**：
```
CreateConversationReq {
    string name = 4;
    repeated string member_id_list = 5;
}
```
没有 `ConversationType type`。服务端按 `member_id_list.size()` 推断私聊/群聊？

**主流做法**：显式 `ConversationType type` 必填，单聊/群聊/频道走不同的服务端逻辑（成员上限、权限模型、命名规则均不同）。

**影响**：私聊和 2 人群在 wire 上不可区分；将来加 CHANNEL 类型必须破坏性变更或加新 RPC。

---

### G9. MarkRead 与 UpdateReadAck 双轨语义重叠

**现状**：
- `ConversationService.MarkRead(conversation_id, seq_id)`（客户端调）
- `MessageService.UpdateReadAck(user_id, conversation_id, seq_id)`（内部调）

两个 RPC 做同一件事，实现层需要决定"谁是 source of truth"。

**主流做法**：单一已读入口。客户端只调 `Conversation.MarkRead`，由 ConversationService 内部级联到 timeline / 未读数计算。

**影响**：实现耦合，容易出现"某些路径漏更新 last_read_seq"的 bug。

---

### G10. Token 生命周期字段缺失

**现状**：
```
LoginRsp { string login_session_id = 2; UserInfo user_info = 3; }
RefreshTokenRsp { string new_session_id = 2; }
```
没有 `expires_at_ms` / `expires_in`，没有区分 access_token 和 refresh_token。

**主流做法**（OAuth2 标准）：
- `access_token`（短期，分钟级）+ `refresh_token`（长期）
- `expires_in_sec` 让客户端自主决定何时主动 refresh
- `token_type`（Bearer / MAC）

**影响**：客户端只能"被动等失败再 refresh"；无法做提前刷新；session_id 永不过期会有安全风险。

---

### G11. `UserSeqPair` 在两个 proto 中重复定义

**现状**：
- `chatnow.push.UserSeqPair`（push_service.proto）
- `chatnow.message.internal.UserSeqPair`（message_internal.proto）

两者字段完全相同。`service-migration-design.md` 自己已提到此冲突。

**主流做法**：迁到 `common/types.proto` 单一定义，两侧 import。

**影响**：跨服务传递时需要手动转换；维护时容易漏改一处。

---

## 三、产品方向相关（非缺陷，但需明确决策）

### P1. ReplyRef 是单层引用，不支持 thread

**现状**：`ReplyRef` 仅含被回复消息的 id + 预览。无 `thread_id` / `thread_root_message_id` / `reply_count`。

**主流分化**：
- Slack/Discord/飞书：thread root + 子讨论 chain（高优）
- 微信/QQ：单层引用即可（低优）
- Telegram：reply_to_top_message_id（兼容两种）

**决策点**：是否做 thread 影响 message 表结构和会话视图。**当前 proto 默认走"无 thread"路线**，建议显式记录。

---

### P2. 端到端加密（E2EE）字段未预留

**现状**：`MessageContent` 全明文 oneof。无 `encrypted_content` / `encryption_version` / `key_id` 字段。

**主流做法**（Signal/WhatsApp/iMessage day 1 设计）：
- `MessageContent` 有 `oneof { plaintext, encrypted_envelope }`
- 服务端不解析 encrypted 部分

**影响**：若未来产品方向需要 E2EE，需要破坏性 wire format 迁移。

**决策点**：现在预留 1 个 `encrypted_content bytes` 字段成本极低（占用一个 field number），延后预留则迁移代价大。

---

### P3. CHANNEL 类型的频道字段未设计

**现状**：`ConversationType.CHANNEL = 3` 已声明，但 `Conversation` 实体无 `is_broadcast / subscriber_count / forward_restricted / channel_username` 等字段。`proto-redesign.md` 第 15 节明确"留待群聊成熟后"。

**决策点**：确认是否纳入此次 v1，还是延后单独 spec。

---

### P4. Bot / Webhook 接口缺失

**现状**：无 `BotService` 或 webhook 注册接口。

**主流做法**：Slack apps、Telegram bots、Discord bots、飞书机器人，全部 day 1 设计。

**决策点**：产品定位是否含开放平台。

---

### P5. SystemNotice 无 i18n

**现状**：`SystemNoticeContent { string text = 1; string notice_type = 2; }` — text 是预渲染明文。

**主流做法**：`notice_type` + `params` map，客户端按 locale 模板化渲染。

**影响**：服务端硬编码语言，多语言场景必改。

---

## 四、可延后（轻量 / 低优）

### L1. brpc Streaming 未使用

推送/同步场景天然 server-streaming，但 Push 走 WS 旁路。工程上 OK，仅记录。

### L2. ResponseHeader 未复用 trace_id

`ResponseHeader.request_id` 由调用方填，未与 brpc trace_id / OpenTelemetry trace context 关联。链路追踪需手动接驳。

### L3. PageResponse.total_count 注释为 "可能近似"

注释只在 `proto-redesign.md` 中，落地 proto 没有 doc string。运行期无法判断该字段是否可信。

### L4. SearchMessages 无高级筛选

只有 `keyword + cursor + limit`，无 `from_user_id / time_range / message_type` 过滤。主流搜索都支持。延后即可。

---

## 五、汇总

| 等级 | 数量 | 项 |
|---|---|---|
| 必须修复 | 3 | G1（鉴权抽离 metadata）、G2（文件预签名 URL）、G3（Device 模型） |
| 建议修复 | 8 | G4–G11 |
| 产品决策 | 5 | P1–P5 |
| 可延后 | 4 | L1–L4 |
| **合计** | **20** | |

---

## 六、下一步

1. 与负责人逐条确认每项的处置（必修 / 建议修 / 延后 / 不做）
2. 对纳入修订的项，开 `2026-05-14-proto-redesign-v2.md` 写具体 wire format 修改方案
3. 评估对 `service-migration-design.md` 的连锁影响（如鉴权抽离会影响所有 handler 的代码模板）
