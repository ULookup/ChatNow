# ChatNow 测试用例目录（补充设计）

> **状态**: 设计完成，待评审
> **日期**: 2026-07-08
> **范围**: 在 `2026-07-08-go-testing-design.md` 基础上，细化每服务/每分类应有哪些测试用例
> **基线**: 现有 102 个测试函数（L2 func 90 + L3 scenario 3 + L4 perf 5 + setup 4）

---

## 0. 优先级定义

| 优先级 | 含义 | 必须在哪个 Phase 完成 |
|---|---|---|
| **P0** | 核心链路 / 未测试 API / 严重错误路径 | Phase 1 |
| **P1** | 重要错误路径 / 边界 / 数据一致性 | Phase 1-2 |
| **P2** | 边角 case / 非功能性 | Phase 2-3 |

---

## 1. 未测试 API 清单（P0，最高优先级）

以下 API 在现有测试中**完全未覆盖**，必须在 Phase 1 补齐：

### 1.1 media 服务（5 个 API 未测试）

| API | 测试用例 | 说明 |
|---|---|---|
| `CompleteUpload` | `TestCompleteUpload_Success` | apply -> PUT MinIO -> complete 全链程，验证 file_id 可用 |
| `CompleteUpload` | `TestCompleteUpload_NotUploaded` | 未 PUT 到 MinIO 就 complete，HEAD 失败 |
| `CompleteUpload` | `TestCompleteUpload_AlreadyCompleted` | 重复 complete，幂等返回 |
| `InitMultipartUpload` | `TestInitMultipart_Success` | 大文件初始化，返回 upload_id + part URLs |
| `InitMultipartUpload` | `TestInitMultipart_FileTooLarge` | 超配额文件拒绝 |
| `ApplyPartUpload` | `TestApplyPartUpload_Success` | 获取分片 presigned URL |
| `CompleteMultipartUpload` | `TestCompleteMultipart_FullFlow` | init -> upload 3 parts -> complete，验证合并后内容 |
| `CompleteMultipartUpload` | `TestCompleteMultipart_MissingPart` | 缺少某个 part number，拒绝 |
| `AbortMultipartUpload` | `TestAbortMultipart_Success` | init -> abort，验证 upload_id 失效 |
| `AbortMultipartUpload` | `TestAbortMultipart_AlreadyAborted` | 重复 abort 幂等 |

### 1.2 message 服务（2 个 API 未测试）

| API | 测试用例 | 说明 |
|---|---|---|
| `SelectByClientMsgId` | `TestSelectByClientMsgId_Found` | 发消息后按 client_msg_id 查询，返回 message |
| `SelectByClientMsgId` | `TestSelectByClientMsgId_NotFound` | 不存在的 client_msg_id，返回空 |
| `UpdateReadAck` | `TestUpdateReadAck_Success` | 更新 last_read_msg_id，影响未读计数 |
| `UpdateReadAck` | `TestUpdateReadAck_Idempotent` | 重复 ACK 相同 msg_id，不回退未读 |

### 1.3 conversation 服务（1 个 API 未测试）

| API | 测试用例 | 说明 |
|---|---|---|
| `GetMemberIds` | `TestGetMemberIds_Success` | 内部 API，返回会话成员 ID 列表 |
| `GetMemberIds` | `TestGetMemberIds_NotMember` | 非成员调用，拒绝 |

---

## 2. 各服务补充测试用例

### 2.1 identity 服务（现有 19 个，补充 12 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| ID-E01 | `TestLogin_Email_Success` | happy path | P0 | 邮箱验证码登录全流程 |
| ID-E02 | `TestLogin_Email_InvalidCode` | error path | P0 | 错误验证码 |
| ID-E03 | `TestSendVerifyCode_RateLimit` | error path | P1 | 同邮箱 60s 内重复发送被限流 |
| ID-E04 | `TestSendVerifyCode_ExpiredCode` | error path | P1 | 验证码过期后使用 |
| ID-E05 | `TestRefreshToken_Expired` | error path | P0 | refresh_token 过期 |
| ID-E06 | `TestLogin_MultiDevice_KickOld` | 状态转换 | P1 | 同用户新设备登录，旧设备 token 失效 |
| ID-E07 | `TestLogout_TokenBlacklisted` | 状态转换 | P1 | 登出后旧 token 不可用 |
| ID-E08 | `TestUpdateProfile_AvatarUpload` | happy path | P1 | 上传头像后更新 profile.avatar_id |
| ID-E09 | `TestUpdateProfile_NicknameTooLong` | 边界 | P1 | 超长昵称拒绝 |
| ID-E10 | `TestRegister_SqlInjection` | 安全 | P1 | nickname 含 SQL 注入字符 |
| ID-E11 | `TestSearchUsers_EmptyKeyword` | 边界 | P2 | 空关键字返回空 |
| ID-E12 | `TestGetMultiUserInfo_PartialNotFound` | 边界 | P2 | 批量查询部分 ID 不存在 |

### 2.2 relationship 服务（现有 15 个，补充 8 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| RL-E01 | `TestSendFriendRequest_Self` | error path | P0 | 不能加自己为好友 |
| RL-E02 | `TestHandleFriendRequest_Expired` | error path | P1 | 申请已过期/已处理 |
| RL-E03 | `TestHandleFriendRequest_NotTarget` | error path | P1 | 非被申请者处理申请 |
| RL-E04 | `TestRemoveFriend_AlsoRemoveConversation` | 数据一致性 | P1 | 删好友后会话是否保留/隐藏 |
| RL-E05 | `TestBlockUser_AlreadyBlocked` | 幂等 | P1 | 重复拉黑幂等 |
| RL-E06 | `TestBlockUser_ThenSendFriendRequest` | error path | P1 | 拉黑后不能再加好友 |
| RL-E07 | `TestListFriends_Pagination` | 边界 | P2 | 分页边界 |
| RL-E08 | `TestSearchFriends_NoMatch` | 边界 | P2 | 无匹配结果 |

### 2.3 conversation 服务（现有 22 个，补充 10 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| CV-E01 | `TestCreateConversation_TooManyMembers` | 边界 | P1 | 成员数超上限（如 >500）拒绝 |
| CV-E02 | `TestAddMembers_DuplicateMember` | error path | P1 | 添加已是成员的用户 |
| CV-E03 | `TestAddMembers_BlockedUser` | error path | P1 | 添加被拉黑的用户 |
| CV-E04 | `TestRemoveMembers_LastOwner` | error path | P0 | 群主不能退出/被移除（需先转让） |
| CV-E05 | `TestTransferOwner_ToNonMember` | error path | P0 | 转让给非成员 |
| CV-E06 | `TestChangeMemberRole_DegradeOwner` | error path | P1 | 不能降级群主为普通成员 |
| CV-E07 | `TestQuitConversation_OwnerAutoTransfer` | 状态转换 | P1 | 群主退出自动转让给最早加入的成员 |
| CV-E08 | `TestDismissConversation_AlreadyDismissed` | 幂等 | P2 | 重复解散 |
| CV-E09 | `TestSetMute_DismissedConversation` | error path | P2 | 对已解散会话操作 |
| CV-E10 | `TestMarkRead_NonMember` | error path | P1 | 非成员标记已读 |

### 2.4 message 服务（现有 14 个，补充 14 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| MS-E01 | `TestSyncMessages_NotMember` | error path | P0 | 非成员同步消息 |
| MS-E02 | `TestSyncMessages_Pagination` | 边界 | P1 | limit 边界，after_seq 游标 |
| MS-E03 | `TestGetHistory_BeforeSeq` | 边界 | P1 | before_seq 分页 |
| MS-E04 | `TestSearchMessages_NoResult` | 边界 | P1 | 无匹配 |
| MS-E05 | `TestSearchMessages_SpecialChars` | 安全 | P1 | 含特殊字符的搜索词 |
| MS-E06 | `TestRecallMessage_ByNonAuthor` | error path | P0 | 非发送者撤回 |
| MS-E07 | `TestRecallMessage_Timeout` | error path | P1 | 超过撤回时限（如 2 分钟） |
| MS-E08 | `TestAddReaction_Duplicate` | 幂等 | P1 | 重复 reaction |
| MS-E09 | `TestAddReaction_OnRecalledMessage` | error path | P1 | 对已撤回消息 reaction |
| MS-E10 | `TestDeleteMessages_NotOwned` | error path | P0 | 删除他人消息 |
| MS-E11 | `TestClearConversation_NonMember` | error path | P1 | 非成员清空会话 |
| MS-E12 | `TestGetReactions_MultiUser` | happy path | P2 | 多用户不同 emoji reaction |
| MS-E13 | `TestListPinnedMessages_Empty` | 边界 | P2 | 无置顶消息 |
| MS-E14 | `TestDeleteMessages_AlreadyDeleted` | 幂等 | P2 | 重复删除 |

### 2.5 transmite 服务（现有 14 个，补充 8 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| TM-E01 | `TestSendMessage_LargeGroup_ReadDiffusion` | 分支 | P0 | >=200 成员走读扩散，仅写 message 主表 |
| TM-E02 | `TestSendMessage_RateLimited` | error path | P1 | 限流触发，返回错误码 |
| TM-E03 | `TestSendMessage_MQFailure_NoResponse` | 可靠性 | P0 | MQ 投递失败，响应 success=false |
| TM-E04 | `TestSendMessage_DismissedConversation` | error path | P0 | 向已解散会话发消息 |
| TM-E05 | `TestSendMessage_EmptyContent` | error path | P1 | 文本消息内容为空 |
| TM-E06 | `TestSendMessage_ContentTooLong` | 边界 | P1 | 文本内容超限（如 >10KB） |
| TM-E07 | `TestSendMessage_FileMessage_BadFileId` | error path | P1 | file_id 不存在 |
| TM-E08 | `TestSendMessage_MentionNonMember` | error path | P2 | @非会话成员 |

### 2.6 media 服务（现有 6 个，补充 18 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| MD-E01 | `TestCompleteUpload_Success` | happy path | P0 | **见 1.1** |
| MD-E02 | `TestCompleteUpload_NotUploaded` | error path | P0 | **见 1.1** |
| MD-E03 | `TestCompleteUpload_AlreadyCompleted` | 幂等 | P1 | **见 1.1** |
| MD-E04 | `TestInitMultipart_Success` | happy path | P0 | **见 1.1** |
| MD-E05 | `TestInitMultipart_FileTooLarge` | error path | P1 | **见 1.1** |
| MD-E06 | `TestApplyPartUpload_Success` | happy path | P0 | **见 1.1** |
| MD-E07 | `TestCompleteMultipart_FullFlow` | happy path | P0 | **见 1.1** |
| MD-E08 | `TestCompleteMultipart_MissingPart` | error path | P1 | **见 1.1** |
| MD-E09 | `TestAbortMultipart_Success` | happy path | P1 | **见 1.1** |
| MD-E10 | `TestAbortMultipart_AlreadyAborted` | 幂等 | P2 | **见 1.1** |
| MD-E11 | `TestApplyUpload_Dedup_SameHash` | 去重 | P0 | 相同 content_hash，第二次 apply 返回相同 file_id |
| MD-E12 | `TestApplyUpload_QuotaExceeded` | 配额 | P0 | 超用户配额拒绝 |
| MD-E13 | `TestApplyUpload_QuotaRemaining` | 配额 | P1 | 配额接近上限边界 |
| MD-E14 | `TestApplyDownload_Success` | happy path | P0 | 上传后下载，验证内容一致 |
| MD-E15 | `TestApplyDownload_OtherUser` | error path | P1 | 非上传者下载私聊文件 |
| MD-E16 | `TestGetFileInfo_Success` | happy path | P1 | 上传后查询 file_info |
| MD-E17 | `TestSpeechRecognition_InvalidAudio` | error path | P1 | 非 PCM 数据 |
| MD-E18 | `TestSpeechRecognition_EmptyContent` | error path | P1 | 空音频数据 |

### 2.7 presence 服务（现有 7 个，补充 8 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| PR-E01 | `TestGetPresence_MultiDevice` | 状态转换 | P1 | 同用户多设备在线，presence 为 online |
| PR-E02 | `TestPresence_HeartbeatRefresh` | 状态转换 | P1 | 心跳续期，TTL 刷新 |
| PR-E03 | `TestPresence_OfflineOnDisconnect` | 状态转换 | P1 | 断开后 presence 变 offline |
| PR-E04 | `TestSubscribePresence_NotificationDelivery` | WebSocket | P0 | 订阅后目标上线，WS 收到通知 |
| PR-E05 | `TestSendTyping_NotFriend` | error path | P1 | 给非好友发 typing |
| PR-E06 | `TestSendTyping_DismissedConversation` | error path | P2 | 给已解散会话发 typing |
| PR-E07 | `TestBatchGetPresence_MixedOnlineOffline` | 边界 | P2 | 部分在线部分离线 |
| PR-E08 | `TestUnsubscribePresence_NotSubscribed` | 幂等 | P2 | 未订阅就取消 |

### 2.8 auth_middleware 服务（现有 5 个，补充 5 个）

| 用例 ID | 名称 | 类别 | 优先级 | 说明 |
|---|---|---|---|---|
| AM-E01 | `TestJWTRequired_MalformedToken` | error path | P0 | 格式错误的 token |
| AM-E02 | `TestJWTRequired_WrongSignature` | error path | P0 | 签名不匹配 |
| AM-E03 | `TestRefreshToken_AsAccessToken` | error path | P1 | refresh_token 当 access_token 用 |
| AM-E04 | `TestJWTRequired_WhitelistedPath` | happy path | P1 | 白名单路径无需 token |
| AM-E05 | `TestAuth_RateLimitOnLogin` | 限流 | P2 | 登录接口限流 |

---

## 3. L3 场景测试补充（现有 3 个，补充 5 个）

| 场景 ID | 名称 | 优先级 | 链路 | 验证点 |
|---|---|---|---|---|
| SC-04 | `TestScenario_OfflineMessageSync` | P0 | u2 离线 -> u1 发 3 条 -> u2 上线 sync -> 验证 3 条按序 | 离线消息不丢、按序、不重复推送 |
| SC-05 | `TestScenario_MediaUploadFullFlow` | P0 | apply -> PUT MinIO -> complete -> download -> 验证内容 -> 重复上传 dedup | 三步上传全链路 + 去重 + 配额 |
| SC-06 | `TestScenario_MessageReliability` | P0 | 发消息 -> 模拟 MQ 短暂不可用 -> 恢复 -> 验证最终落库 | 消息不丢（Nack requeue） |
| SC-07 | `TestScenario_MultiDeviceLogin` | P1 | u1 设备 A 登录 -> 设备 B 登录 -> A 被踢 -> A token 失效 | 多设备踢人一致性 |
| SC-08 | `TestScenario_LargeGroupFanOut` | P1 | 200+ 成员群 -> 发消息 -> 验证读扩散（仅写主表） -> 各成员 sync | 大群读扩散正确性 |

### 场景 4 详细设计：离线消息同步

```
预置：u1, u2 好友 + 单聊会话
步骤：
  1. u2 登录后立即 logout（模拟离线）
  2. u1 发 3 条消息（text/image/text）
  3. u2 重新登录 -> SyncMessages(after_seq=0)
  4. 验证返回 3 条消息，seq 递增
  5. u2 开 WS -> 不应收到旧消息推送（已通过 sync 拉取）
  6. u1 再发 1 条 -> u2 WS 收到 CHAT_MESSAGE_NOTIFY
  7. u2 SyncMessages(after_seq=上一步 seq) -> 仅返回新 1 条
验证：消息不丢、按序、不重复
```

### 场景 5 详细设计：媒体三步上传全链路

```
预置：u1 登录
步骤：
  1. ApplyUpload(image/jpeg, 1024 bytes, content_hash=H1)
  2. PUT 到 MinIO presigned URL（用 Go net/http client）
  3. CompleteUpload -> 验证 success
  4. ApplyDownload(file_id) -> 下载 -> 验证内容与上传一致
  5. 再次 ApplyUpload(相同 content_hash=H1) -> 验证返回相同 file_id（dedup）
  6. 直查 DB：media_blob_ref ref_count=2
  7. 上传大文件（>5MB）走 multipart：InitMultipart -> ApplyPartUpload x3 -> CompleteMultipart
  8. 下载大文件 -> 验证合并后内容完整
验证：三步上传 + 去重 + multipart + 下载一致性
```

### 场景 6 详细设计：消息可靠性

```
预置：u1, u2 好友 + 单聊会话
步骤：
  1. docker compose stop rabbitmq（模拟 MQ 不可用）
  2. u1 发消息 -> 验证响应 success=false（MQ 投递失败）
  3. docker compose start rabbitmq
  4. wait_for_services.sh 等待恢复
  5. u1 用相同 client_msg_id 重发 -> 验证 success=true
  6. u2 SyncMessages -> 验证收到该消息
  7. 直查 DB：message 表有 1 条（不重复）
验证：MQ 失败不丢消息、client_msg_id 幂等去重
```

---

## 4. 跨服务测试分类（新增）

### 4.1 WebSocket 实时推送测试（P0，当前完全缺失）

现有测试全走 HTTP，未验证 WebSocket 推送。需新增 `tests/func/ws_notify_test.go`：

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| WS-01 | `TestWS_NewMessageNotify` | P0 | 发消息后，接收方 WS 收到 CHAT_MESSAGE_NOTIFY |
| WS-02 | `TestWS_FriendRequestNotify` | P0 | 好友申请后，被申请方 WS 收到通知 |
| WS-03 | `TestWS_FriendAcceptNotify` | P1 | 好友申请通过后，申请方 WS 收到通知 |
| WS-04 | `TestWS_ConversationCreateNotify` | P1 | 会话创建后，成员 WS 收到通知 |
| WS-05 | `TestWS_PresenceChangeNotify` | P1 | 订阅的用户上线/离线，WS 收到通知 |
| WS-06 | `TestWS_Reconnect` | P1 | WS 断开后重连，遗漏消息通过 sync 补齐 |
| WS-07 | `TestWS_TypingNotify` | P2 | typing 通知送达订阅者 |

需先在 `tests/pkg/client/` 新增 `ws.go`（WebSocket 客户端封装）。

### 4.2 数据一致性测试（P0，当前完全缺失）

现有测试仅验证 HTTP 响应，不验证 DB/ES 实际落库。需新增 `tests/pkg/verify/` 包：

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| DC-01 | `TestConsistency_MessageWriteDiffusion` | P0 | 发消息后直查 DB：message 表 1 行 + user_timeline N 行（N=成员数） |
| DC-02 | `TestConsistency_ESIndexSync` | P0 | 文本消息发后直查 ES：索引有文档，内容匹配 |
| DC-03 | `TestConsistency_UnreadCount` | P0 | 发消息后直查 DB：接收方 user_timeline.last_read_msg 与未读计数一致 |
| DC-04 | `TestConsistency_RecallMessage` | P1 | 撤回后直查 DB：message.status=RECALLED，timeline 不删 |
| DC-05 | `TestConsistency_DeleteTimeline` | P1 | 用户删聊天记录后直查 DB：user_timeline 删除，message 保留 |
| DC-06 | `TestConsistency_FriendRelation` | P1 | 加好友后直查 DB：relation 表双向各 1 行 |
| DC-07 | `TestConsistency_MediaQuota` | P1 | 上传后直查 DB：media_user_quota 增量正确 |

需在 `tests/pkg/verify/` 新增 `db.go`（MySQL 直查）+ `es.go`（ES 直查）。

### 4.3 并发测试（P1，当前完全缺失）

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| CC-01 | `TestConcurrent_SendMessage_SameClientMsgId` | P0 | 10 goroutine 用相同 client_msg_id 发消息，仅 1 条落库 |
| CC-02 | `TestConcurrent_SendMessage_DifferentMsgId` | P1 | 10 goroutine 并发发消息，全部落库，seq 不重复 |
| CC-03 | `TestConcurrent_FriendAccept_ThenSend` | P1 | 好友通过瞬间并发发消息，不丢 |
| CC-04 | `TestConcurrent_MediaUpload_SameHash` | P1 | 相同 content_hash 并发上传，dedup 正确 |
| CC-05 | `TestConcurrent_Reaction_SameEmoji` | P2 | 多用户同时给同一消息加相同 emoji |

### 4.4 可靠性测试（P1，当前完全缺失）

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| RL-01 | `TestReliability_MQRestart` | P0 | 发消息中途 RabbitMQ 重启，验证消息最终落库 |
| RL-02 | `TestReliability_ServiceRestart` | P1 | message 服务重启，验证消费不丢 |
| RL-03 | `TestReliability_DBReconnect` | P1 | MySQL 短暂断连，验证重连后写入正常 |
| RL-04 | `TestReliability_DeadLetterQueue` | P2 | 消费失败超阈值，消息进死信队列 |

### 4.5 安全测试（P1，当前部分覆盖）

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| SEC-01 | `TestSecurity_AuthBypass_NoToken` | P0 | 无 token 访问受保护接口 |
| SEC-02 | `TestSecurity_AuthBypass_OtherUser` | P0 | 用 A 的 token 访问 B 的数据 |
| SEC-03 | `TestSecurity_SQLInjection_Search` | P1 | 搜索接口 SQL 注入 |
| SEC-04 | `TestSecurity_XSS_MessageContent` | P1 | 消息内容含 XSS payload |
| SEC-05 | `TestSecurity_PathTraversal_FileName` | P1 | 文件名含 `../../etc/passwd` |
| SEC-06 | `TestSecurity_PrivilegeEscalation_MemberToOwner` | P0 | 普通成员尝试改自己为群主 |

### 4.6 限流与配额测试（P1）

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| RL-Q01 | `TestRateLimit_SendMessage_Burst` | P1 | 短时间大量发消息触发限流 |
| RL-Q02 | `TestQuota_MediaUpload_ExceedUserQuota` | P0 | 超用户总配额拒绝 |
| RL-Q03 | `TestQuota_MediaUpload_ExceedSingleFile` | P0 | 单文件超大小限制（已有 TestApplyUpload_FileTooLarge） |
| RL-Q04 | `TestQuota_MediaUpload_CleanupOrphanedBlob` | P2 | abort 后验证 cleanup worker 清理孤儿 blob |

---

## 5. L4 性能测试补充（现有 5 个，补充 3 个）

| 用例 ID | 名称 | 优先级 | 说明 |
|---|---|---|---|
| PF-01 | `BenchmarkGroupMessageFanOut` | P1 | 200 人群发消息吞吐 |
| PF-02 | `BenchmarkMediaUpload` | P1 | 不同文件大小（1KB/1MB/10MB）上传吞吐 |
| PF-03 | `BenchmarkSearchMessages` | P2 | ES 全文检索延迟（100 万消息量级） |

---

## 6. 测试基础设施补充

### 6.1 需新增的 tests/pkg/ 包

| 文件 | 用途 | 依赖 Phase |
|---|---|---|
| `tests/pkg/client/ws.go` | WebSocket 客户端封装（连接/读通知/断线重连） | Phase 1（WS 测试） |
| `tests/pkg/verify/db.go` | MySQL 直查验证 helper（message/timeline/relation 等表） | Phase 1（一致性测试） |
| `tests/pkg/verify/es.go` | ES 直查验证 helper（message 索引检索） | Phase 1（一致性测试） |
| `tests/pkg/verify/minio.go` | MinIO 直查验证 helper（对象存在性/内容比对） | Phase 2（media 测试） |

### 6.2 需新增的 fixture

| 函数 | 用途 | 依赖 Phase |
|---|---|---|
| `fixture.CreateGroup(t, owner, members)` | 快速建群 | Phase 1 |
| `fixture.SendTextMessage(t, client, convID, text)` | 快速发文本消息 | Phase 1 |
| `fixture.UploadFile(t, client, content, mime)` | 完整三步上传返回 file_id | Phase 2 |
| `fixture.ConnectWS(t, client)` | 建立 WS 连接返回 WsClient | Phase 1 |

---

## 7. 统计与分 Phase 汇总

### 7.1 用例数量统计

| 类别 | 现有 | 补充 | 合计 |
|---|---|---|---|
| L2 功能测试 | 90 | 83 | 173 |
| L3 场景测试 | 3 | 5 | 8 |
| L4 性能测试 | 5 | 3 | 8 |
| WebSocket 推送 | 0 | 7 | 7 |
| 数据一致性 | 0 | 7 | 7 |
| 并发 | 0 | 5 | 5 |
| 可靠性 | 0 | 4 | 4 |
| 安全 | 0 | 6 | 6 |
| 限流配额 | 0 | 4 | 4 |
| **合计** | **98** | **124** | **222** |

### 7.2 按 Phase 分配

| Phase | 内容 | 新增用例数 | 优先级范围 |
|---|---|---|---|
| Phase 0 | CI 基础设施 | 0（已有 plan） | - |
| Phase 1 | 核心消息链路（transmite + message + 一致性 + WS + 并发） | ~60 | P0 |
| Phase 2 | media + presence + 安全 + 移除 C++ | ~45 | P0-P1 |
| Phase 3 | 可靠性 + 限流配额 + 性能基线 + 边角 case | ~19 | P1-P2 |

---

## 8. 验收标准

每个 Phase 完成后应满足：

**Phase 1：**
- media 5 个未测试 API 全部覆盖
- message 2 个未测试 API 全部覆盖
- transmite + message 所有 P0 错误路径覆盖
- WebSocket 7 个推送测试通过
- 数据一致性 7 个测试通过
- 离线同步 + 可靠性 2 个场景通过

**Phase 2：**
- media 18 个补充用例全部通过
- presence 8 个补充用例全部通过
- 安全 6 个测试通过
- C++ 测试文件全部移除

**Phase 3：**
- 可靠性 4 个测试通过
- 限流配额 4 个测试通过
- 性能 3 个基准建立基线
- CI nightly 全绿
