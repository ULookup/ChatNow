# ChatNow 即时通讯系统（服务端）

一个用 C++ 编写的分布式 IM 后端，目标是**生产可上线**：在 demo 项目和真正能扛业务的微服务集群之间，按"删掉这个会怎样"的尺度做取舍。

支持单聊 / 群聊、好友关系、媒体上传（含大文件分片）、消息全文检索、离线消息增量补齐、多端在线状态与推送。客户端通过 HTTP（业务请求，走 Gateway）+ WebSocket（服务端推送，直连 Push 服务）与服务端通信。

> **当前阶段**：3.0 重构线，已落地 P1（横切错误处理 / 鉴权元数据 / 结构化日志）、P2（JWT 多端鉴权）、P4（MinIO 对象存储）、P8（trace_id 全链路）。后续 P3 / P5 / P6 / P7 会继续覆盖 Device 模型、Conversation 拆分、Message 重构、运营工具。详见 `docs/superpowers/specs/`。

---

## 目录

- [一、整体架构](#一整体架构)
- [二、横切基础设施（P1 / P2 / P4 / P8）](#二横切基础设施p1--p2--p4--p8)
- [三、技术栈](#三技术栈)
- [四、目录结构](#四目录结构)
- [五、各服务说明](#五各服务说明)
- [六、数据存储](#六数据存储)
- [七、消息流转链路](#七消息流转链路)
- [八、构建与部署](#八构建与部署)
- [九、配置说明](#九配置说明)
- [十、文档索引](#十文档索引)

---

## 一、整体架构

```
                    ┌────────────────────────────────┐
       客户端  ───→ │  Gateway （HTTP 9000）         │
       (HTTP)       │  鉴权 / 路由（无状态化）        │
                    └───────────────┬────────────────┘
                                    │ brpc + Protobuf
                                    │ (透传 metadata: x-user-id /
                                    │  x-device-id / x-trace-id / x-jwt-jti)
                                    ▼
   ┌───────────┬───────────┬──────────────┬──────────┬──────────┬──────────────────────────┐
   ▼           ▼           ▼              ▼          ▼          ▼                          ▼
 Identity   Friend    ChatSession    Transmite   Message    Media        Push (含 WS 9001)
 (登录/JWT) (关系/申请) (会话/成员)   (发送入口)   (存储/检索) (对象存储+ASR)  长连接 / 推送
                                          │                                  ↑
                                          ▼                                  │
                                     RabbitMQ                              客户端 (WebSocket)
                                       chat_msg_exchange
                                          │
                                          ▼
                                     msg_queue_db
                                          │
                                  Message DB Consumer
                                  写 message + user_timeline (一个 ODB 事务)
                                          │
                                          ▼ (commit 后 fire-and-forget)
                                     es_index_exchange ──→ es_index_queue
                                          │                     │
                                          │              Message ES Consumer
                                          │              写 ES 索引（仅文本）
                                          │
                                          └─→ msg_push_queue ─→ Push 服务 ─→ WebSocket 下发
```

**架构关键点**

1. **HTTP 与 WebSocket 入口分离** —— Gateway 仅终结 HTTP（业务请求 + JWT 鉴权后注入 metadata），WebSocket 长连接由 Push 服务在 9001 终结并独立做首帧鉴权。后端业务服务在内网只校验 metadata 存在，不再解 JWT，详见 §2。
2. **服务发现走 etcd** —— 各服务启动后注册到 `/service/<svc>/instance/<id>`；调用方用 `Discovery + ServiceManager` 自动发现可用 brpc Channel，轮询负载均衡。
3. **proto 业务体干净** —— 业务 Req/Rsp 不再带 `user_id / session_id / trace_id`，全部走 metadata；这点是 P1 / P8 联合落地的硬规约。
4. **媒体不过 bytes** —— Media 服务签 presigned URL 让客户端直传 MinIO；服务端只过元数据 + 配额，详见 §2.4。
5. **写扩散 + 串行 ES 索引** —— Transmite 投递 `chat_msg_exchange`，Message DB Consumer 在一个 ODB 事务里写 `message` + `user_timeline`（每个成员一行）；**事务 commit 后**才向 `es_index_exchange` 投递 `ESIndexEvent`，由 ES Consumer 写索引；失败落 `ESOutbox`（Redis ZSET）+ reaper 重投。这样不存在"ES 有而 DB 无"的不一致。
6. **错误码统一** —— `proto/common/error.proto` 是真值源，C++ 镜像在 `common/error/error_codes.hpp`；handler 一律 `throw ServiceError(code, msg)`，由 `HANDLE_RPC` 宏统一捕获。

---

## 二、横切基础设施（P1 / P2 / P4 / P8）

3.0 重构的核心：**先把基础设施做对**，业务再迭代。

### 2.1 错误处理 + 鉴权元数据 + 结构化日志（P1）

源码：`common/error/`、`common/auth/`、`common/log/`、`common/infra/log_json.hpp`

- **`ServiceError(code, msg)`** —— 业务异常基类；handler 一律 throw，不手填 ResponseHeader。
- **`HANDLE_RPC(cntl, req, rsp, { body })` 宏** —— 自动：
  1. `extract_auth(cntl)` 解析 metadata（x-user-id / x-device-id / x-trace-id / x-jwt-jti）；缺失抛 `kSystemInternalError`；
  2. `LogContext::set` 把 trace 字段写入 bthread-local；
  3. 先写成功 ResponseHeader，再执行 body；
  4. body 抛 `ServiceError` → 覆盖为失败 + WARN 日志；抛其它异常 → 9001 + ERROR 日志（不泄漏 what）；
  5. finally 清理 LogContext。
- **`AuthContext`**（`common/auth/auth_context.hpp`）—— 保存 user_id / device_id / trace_id / jwt_jti，body 内可直接用 `auth.user_id`。
- **`forward_auth_metadata`**（`common/auth/forward_auth.hpp`）—— 内部服务间二跳调用时透传 metadata。
- **结构化日志** —— `common/infra/log_json.hpp` + spdlog；输出 JSON 行，含 `service_name / level / ts / trace_id / user_id / device_id / msg + 字段`。

### 2.2 JWT 多端鉴权（P2）

源码：`common/auth/jwt_codec.hpp`、`jwt_store.hpp`、`auth_config_loader.hpp`

- **HS256**（仅此一种），多 `kid` 支持（密钥轮换不停机）。
- **JwtCodec** —— 签发 access / refresh，验证签名 + 过期 + jti。
- **JwtStore（Redis）** —— 黑名单 / refresh 反查 / refresh 重放检测（SETNX）。
- **加载** —— `load_jwt_config_from_file("conf/auth.json")`，fail-fast。
- **运维** —— `docs/operations/jwt-key-rotation.md` 详细记录密钥轮换 runbook。

```json
// conf/auth.json
{ "auth": { "jwt": {
  "current_kid": "v1",
  "keys": { "v1": "<>=32 字节字符串>" },
  "access_ttl_sec": 7200,
  "refresh_ttl_sec": 2592000
}}}
```

接入路径：`IdentityService.Login / Logout / RefreshToken`（`proto/identity/identity_service.proto`），服务端实现在 `user/source/`，Gateway 中间件在 `gateway/source/gateway_auth.hpp`。

### 2.3 trace_id 全链路（P8）

源码：`common/utils/trace_id.hpp`、`common/log/log_context.hpp`、`common/mq/`

- 32 char 小写 hex 全局唯一，由 Gateway 生成或客户端透传。
- HTTP header `x-trace-id` → brpc metadata → handler 内 `auth.trace_id` → 结构化日志字段 → MQ AMQP header → 消费者再 set 回 LogContext。
- 监控/排障路径：日志查询直接按 `trace_id` 过滤即可串起一次请求。

### 2.4 MinIO 对象存储（P4）

源码：`common/infra/s3_client.hpp`、`common/utils/{content_hash,object_key,magic_sniff,mime_whitelist,avatar_url}.hpp`、`file/source/`、`odb/media_*.hxx`、`common/dao/mysql_media_*.hpp`

替换原本"file/ 写本地磁盘 + bytes 走 RPC"的简陋实现，改为：

| 能力 | 实现 |
|---|---|
| 单段三步上传 | `ApplyUpload` → 客户端 PUT presigned URL → `CompleteUpload` |
| 大文件分片 | `InitMultipart` → `ApplyPart` × N → `CompleteMultipart` / `AbortMultipart` |
| 下载 | `ApplyDownload` 签 presigned GET（私密 bucket） |
| 头像 / sticker | 公共 bucket + anonymous download，URL 永久可达，无需签名 |
| 短音频 ASR | `SpeechRecognition` 保留 RPC + bytes 通道（待接 ASR） |
| **去重** | `content_hash = sha256:<64hex>`，多 file_id 共享同一物理对象，`ref_count` 跟踪 |
| **配额** | 用户级 5 GB（默认），`media_user_quota` 表，`used + pending + new ≤ quota` 校验 |
| **mime 白名单** | JSON 配置 + size 上限：image*(20MB) / video(500MB) / audio(50MB) / pdf(100MB) / text(5MB) |
| **危险类拦截** | magic sniff 检测 PE/ELF/Mach-O，伪装成图片直接置 quarantined |
| **垃圾回收** | 内嵌 `CleanupWorker`：5 类任务（pending 超时 / multipart 孤儿 / 未引用 blob GC / quarantined / magic sniff），Redis lease 单实例租约 |

**两个 bucket**：
- `chatnow-media-public` —— 头像 / 群头像 / sticker，匿名可读，CDN 友好
- `chatnow-media-private` —— 聊天媒体，全部走 presigned URL

详细 spec 见 `docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §3。

---

## 三、技术栈

| 类别 | 选型 | 备注 |
|---|---|---|
| 语言 / 标准 | C++17 | 多处用到 nested namespace / string_view / inline header |
| RPC | brpc + Protobuf 3 | HTTP+JSON 入站同时启用 |
| 数据库 | MySQL 8.0 | ODB ORM，schema 由 `--generate-schema` 生成 |
| 缓存 | Redis 7 | redis++（hiredis）|
| 检索 | Elasticsearch 7 | elasticlient |
| 消息队列 | RabbitMQ | AMQP-CPP + libev，`publish_confirm` 异步带 ACK |
| 对象存储 | MinIO（S3 兼容）| aws-sdk-cpp v1.11.420（仅 S3 module）|
| 服务发现 | etcd | etcd-cpp-api，监听 `/service/*` 维护信道 |
| 鉴权 | jwt-cpp v0.7.0（HS256）| picojson |
| 日志 | spdlog（JSON 输出）| 自实现 `log_json` |
| ID 生成 | Snowflake | `common/infra/snowflake.hpp`，`Next() → uint64_t` |
| 邮件 | libcurl + SMTP | 注册 / 找回 |
| 容器 | Docker + docker-compose | minio/mysql/redis/etcd/rabbitmq |

---

## 四、目录结构

```
ChatNow/
├── proto/                         # 全部 proto 契约（按域分目录）
│   ├── common/                    # envelope / error / types
│   ├── identity/                  # IdentityService（登录 / JWT / 资料）
│   ├── conversation/              # 会话域（拆分自老 chatsession.proto）
│   ├── relationship/              # 好友 / 申请 / 黑名单
│   ├── message/                   # 消息域
│   ├── media/                     # MediaService（P4）
│   ├── presence/                  # 在线状态
│   ├── transmite/                 # 消息转发入口
│   └── push/                      # 推送通知
│
├── common/                        # 公共组件（header-only 优先）
│   ├── auth/                      # AuthContext / JwtCodec / JwtStore / forward_auth
│   ├── error/                     # ServiceError / HANDLE_RPC / error_codes
│   ├── log/                       # LogContext (bthread-local)
│   ├── infra/                     # logger / log_json / etcd / s3_client / snowflake / mail
│   ├── dao/                       # MySQL ODB DAO 包装 + Redis / ES helper
│   ├── mq/                        # RabbitMQ Channel + Publisher + Consumer
│   ├── utils/                     # trace_id / content_hash / object_key /
│   │                              # magic_sniff / mime_whitelist / avatar_url 等
│   └── test/                      # 公共组件单元测试（GLOB test_*.cc）
│
├── odb/                           # ODB schema 源（*.hxx，pragma db）
│   ├── user.hxx / message.hxx / chat_session*.hxx / friend_apply.hxx ...
│   └── media_file.hxx / media_blob_ref.hxx / media_user_quota.hxx (P4)
│
├── gateway/                       # 网关：HTTP 入口 + JWT 鉴权中间件（无状态化，不持长连接）
├── user/                          # IdentityService + UserService（注册/资料/搜索）
├── friend/                        # RelationshipService（好友 / 申请）
├── chatsession/                   # ConversationService（会话 / 成员）
├── transmite/                     # 消息转发：组装 + 投递 MQ
├── message/                       # 消息存储 + 检索 + 双消费者
├── file/                          # MediaService（P4 重写为对象存储）
│   └── source/                    # media_main / media_server / *_handler / cleanup_worker
├── push/                          # PushService（在线状态 + 推送 outbox）
│
├── conf/                          # gflags flagfile + JSON 配置
│   ├── auth.json                  # JWT 多 kid（P2）
│   ├── media.json                 # bucket / mime 白名单 / presign（P4）
│   └── *_server.conf              # 各服务运行参数
│
├── sql/V4__media.sql              # P4 媒体三表参考 DDL（权威由 ODB 生成）
├── docker/                        # docker-compose（minio + minio-init）
├── scripts/install_aws_sdk_linux.sh  # P4 依赖安装
├── docker-compose.yml             # 顶层：基础设施 + 各服务（旧）
├── docs/                          # 架构 / spec / 实施计划 / 运维 runbook
└── CMakeLists.txt
```

---

## 五、各服务说明

### Gateway（HTTP 9000，无状态化）

源码：`gateway/source/`

- **HTTP 唯一入口**：所有业务请求（注册 / 登录 / 资料 / 好友 / 会话 / 消息 / 媒体 / 推送通知发起），按路径分发到对应后端 brpc 服务。
- **JWT 鉴权中间件**（`gateway_auth.hpp`，P2）：
  - 解析 `Authorization: Bearer <jwt>`；
  - 校验签名 + 过期 + 黑名单（Redis）；
  - 通过后把 `user_id / device_id / trace_id / jti` 注入下游 brpc HTTP metadata。
- **trace_id**（`gateway_setup_trace`，P8）：客户端无 `x-trace-id` 时生成 32 hex；写入 LogContext + 透传下游。
- **不持有长连接**：Gateway 完全无状态化，WebSocket 终结在 Push 服务（见下文 Push 章节）。Gateway / 业务服务向客户端发通知统一走 `PushService.PushToUser`，由 Push 服务负责 fanout 到 WebSocket。

### IdentityService + UserService（端口 10003）

源码：`user/source/`，proto: `proto/identity/identity_service.proto` + `proto/user.proto`

| RPC | 用途 |
|---|---|
| `Register` | 用户名 / 邮箱验证码注册 |
| `Login` | 签发 access + refresh JWT；写 Redis presence |
| `Logout` | access 加黑、refresh 撤销 |
| `RefreshToken` | 重放检测后换新 access；老 refresh 立即失效 |
| `SendVerifyCode` | 邮箱验证码（SMTP 163），写 Redis Codes |
| `GetProfile` / `UpdateProfile` | 资料；avatar 走 file_id（P4 改造，见 §2.4） |
| `SearchUsers` | 走 ES `user` 索引模糊搜索 |
| `GetMultiUserInfo` | 内部批量查询 |

### RelationshipService（端口 10006）

源码：`friend/source/`，proto: `proto/relationship/`

- 好友列表 / 删除 / 申请 / 处理（同意时自动建会话）/ 待处理列表 / 模糊搜索（ES）。
- 同意申请会写 `relation` + 创建 `chat_session` + `chat_session_member`，并通过网关推送 `FRIEND_ADD_PROCESS_NOTIFY` / `CHAT_SESSION_CREATE_NOTIFY`。

### ConversationService（端口内部）

源码：`chatsession/source/`，proto: `proto/conversation/`

接口最丰富的服务，覆盖：
- 会话生命周期：创建 / 列表 / 详情 / 改名 / 改头像 / 状态（NORMAL/ARCHIVED/DISMISSED）/ 搜索
- 成员管理：列表 / 添加 / 移除 / 转让群主 / 改权限 / 退群
- 用户在该会话中的状态：免打扰、置顶、显示/隐藏、未读 ACK
- 内部接口：`GetMemberIdList` 供 Transmite 服务做消息扩散

### Transmite 服务（端口 10004）

源码：`transmite/source/`

接收客户端 `/service/message_transmit/new_message` 请求，核心流程：

1. 并行 brpc：User.GetUserInfo + ChatSession.GetMemberIdList；
2. Snowflake 生成 message_id，组装"胖" `InternalMessage`（含 MessageInfo + member_id_list）；
3. RabbitMQ `publish_confirm` 异步投递；
4. Broker ACK 后再 `done->Run()` 返回成功 + 完整消息体 + 收件人列表（保证不丢消息）；
5. 发布前 `inject_trace_headers` 写 AMQP header（P8）。

Gateway 把 message_id / seq_id 回写给发送方；推送 `CHAT_MESSAGE_NOTIFY` 由 Message 在 DB commit 后投 `msg_push_queue` → Push 服务消费下发，详见 §7。

### Message 服务（端口 10005）

源码：`message/source/`

**两个角色**：

**A. MQ 串行消费者**：

```
chat_msg_exchange ─→ msg_queue_db ─→ onDBMessage
                                       │
                                       │ (DB commit 成功后)
                                       ├─→ es_index_exchange ─→ es_index_queue ─→ onESIndexMessage
                                       │   失败落 ESOutbox（reaper 重投）
                                       └─→ chat_push_exchange ─→ msg_push_queue ─→ Push 服务
                                           失败落 PushOutbox（reaper 重投）
```

- `onDBMessage` —— 一个 ODB 事务里同写 `message` + `user_timeline`（写扩散：会话每个成员一行）；失败 NackRequeue；
- DB commit 后投递轻量 `ESIndexEvent` 到 `es_index_exchange`（不是 transmite 的"双写"路径），同时投递 `msg_push_queue`；
- `onESIndexMessage` —— 仅文本消息写 ES `message` 索引；
- 启动时 `seq` 从 DB 回填 Redis（消除 Redis 重启丢失窗口，见 commit `6e04346`）。

**B. RPC 查询**：
- `GetHistoryMsg` / `GetRecentMsg` / `GetOfflineMsg`（按 `last_message_id` 增量）/ `GetMsgByIds`
- `MsgSearch`（走 ES）
- `DeleteTimelineMsg`（用户删除自己的聊天记录，原始 message 保留）
- `GetUnreadCount`（按 `last_read_msg_id`）

### MediaService（端口 10002，P4 重写）

源码：`file/source/`，proto: `proto/media/media_service.proto`

| RPC | 用途 |
|---|---|
| `ApplyUpload` | 校验 mime + size + quota → 签 PUT presigned URL（含去重） |
| `CompleteUpload` | HEAD 比对 + ref_count++ + quota++ |
| `InitMultipartUpload` / `ApplyPartUpload` / `CompleteMultipartUpload` / `AbortMultipartUpload` | 大文件分片 |
| `ApplyDownload` | 签 GET presigned URL（committed 行才放） |
| `GetFileInfo` | 元数据 |
| `SpeechRecognition` | 短音频 ASR 占位（待接百度 ASR） |

详见 §2.4 + `docs/superpowers/plans/2026-05-14-p4-media-object-storage.md`。

### Push 服务（brpc 内部端口 + WS 9001）

源码：`push/source/`

承担两类职责：

**A. WebSocket 终结**：

- 监听 9001，终结客户端长连接；首帧客户端必须发 `CLIENT_AUTH` 完成鉴权，鉴权通过后写入本实例内存映射 `(user_id, device_id) ↔ ws_hdl`（`push/source/connection.hpp`）。
- 处理客户端心跳与消息推送 ACK；断连清理本地映射 + Redis presence / online 路由。

**B. 推送投递**：

- 消费 `msg_push_queue`（由 Message 服务在 DB commit 后投递）→ 查 Redis 路由 → 命中本实例直接 WS 下发，命中其它实例则跨实例 brpc fanout，由对端实例完成 WS 下发。
- 业务通知（好友申请 / 群事件 / 消息撤回等）通过 `PushToUser` brpc 入口由其它服务（如 Friend / Chatsession）发起。
- 在线状态多设备聚合 + 在线路由维护。
- 失败投递兜底：`PushOutbox` / `CrossInstanceOutbox`（Redis Sorted Set + 单实例 reaper 租约重投）。

---

## 六、数据存储

### MySQL（库名：`chatnow`）

权威 schema 由 ODB 通过 `odb/*.hxx` 的 `--generate-schema` 自动生成。`sql/V4__media.sql` 仅作部署参考。

| 表 | 说明 |
|---|---|
| `user` | 用户主表：user_id / nickname / mail / phone / avatar_id / status |
| `user_device` | 多端登录设备（P3 规划，部分字段已就位） |
| `relation` / `friend_apply` | 好友关系 / 申请事件 |
| `chat_session` / `chat_session_member` | 会话 + 成员个性化（last_read_msg / muted / pin / role） |
| `chat_session_view` | 会话视图（聚合最近一条消息预览） |
| `message` | 消息原始内容 |
| `message_attachment` | 多附件（多图、视频带封面） |
| `message_mention` / `message_reaction` / `message_read` | @ / reaction / 已读 |
| `user_timeline` | 写扩散：每条消息为每个会话成员各一行 |
| `media_file` | 媒体元数据 + 状态机（pending/committed/deleted/quarantined） |
| `media_blob_ref` | 物理 blob 引用计数（按 content_hash 去重） |
| `media_user_quota` | 用户级配额（默认 5 GB） |

### Redis（域命名空间隔离）

| key 前缀 | 用途 |
|---|---|
| `im:sess:` | 登录 session_id → user_id（P2 后逐步弃用，走 JWT） |
| `im:status:` | user_id → 在线状态 |
| `im:code:` | 邮箱验证码 |
| `im:seq:ssid:` / `im:seq:uid:` | 会话级 / 用户级单调 seq |
| `im:last:` | 最后一条消息预览缓存 |
| `im:dev:` | 用户在线设备集合（多端推送用） |
| `im:read:` | 大群已读暂存（落库前缓冲） |
| `im:rl:user:` / `im:rl:ssid:` | 令牌桶限流 |
| `im:online:` / `im:push:route:` | 跨实例推送路由 |
| `im:unack:` | 未确认消息 Sorted Set |
| `im:push:outbox` / `:cross_outbox` | 投递失败兜底（带 reaper 单实例租约） |
| `im:media:gc:lease` | MediaService cleanup worker 单实例租约（P4） |
| `im:auth:bl:` / `im:auth:rt:` | JWT 黑名单 / refresh 反查（P2） |

### Elasticsearch

| 索引 | 用途 |
|---|---|
| `user` | 昵称 / 邮箱 / 签名 模糊搜索 |
| `chat_session` | 会话名搜索 |
| `message` | 仅文本消息全文检索 |

### RabbitMQ

| 资源 | 由谁发布 | 由谁消费 | 用途 |
|---|---|---|---|
| `chat_msg_exchange` → `msg_queue_db` | Transmite | Message `onDBMessage` | DB 落库（message + user_timeline） |
| `es_index_exchange` → `es_index_queue` | Message（DB commit 后） | Message `onESIndexMessage` | ES 索引（仅文本，承载 `ESIndexEvent` 轻量事件） |
| `chat_push_exchange` → `msg_push_queue` | Message（DB commit 后） | Push | 推送 |

设计意图：transmite **不**直接双写 DB queue + ES queue；ES 事件由 DB commit 之后再投递，避免 "ES 有而 DB 无" 的不一致。失败兜底：`ESOutbox` / `PushOutbox`（Redis ZSET）+ 单实例 reaper 租约重投。

AMQP header：发布时注入 `x-trace-id`，消费时由 Message 服务取出 set 回 LogContext（P8 全链路）。

### MinIO（P4）

| bucket | 访问 |
|---|---|
| `chatnow-media-public` | anonymous download；avatar / sticker URL 永久可达 |
| `chatnow-media-private` | 私密；仅签名 GET URL |

object key 格式：
- `chat/<yyyy/mm/dd>/<hash[:2]>/<hash>` —— 私密会话媒体，date+hash 双层分片（冷热分区友好）
- `avatar/<hash>` / `sticker/<hash>` —— 公共资源，hash 直寻址

---

## 七、消息流转链路

群消息发送，完整链路：

```
客户端
  │ 1. HTTP POST /service/message_transmit/new_message
  │    Authorization: Bearer <jwt>
  ▼
Gateway
  │ 2. JWT 校验 → 注入 metadata（user_id/device_id/trace_id/jti）
  │ 3. brpc 调 transmite_service
  ▼
Transmite
  │ 4. 并行：User.GetUserInfo + ChatSession.GetMemberIdList
  │ 5. Snowflake 生成 message_id，组装 InternalMessage
  │ 6. publish_confirm 投递 chat_msg_exchange（AMQP header 含 trace_id）
  │ 7. Broker ACK 后 done->Run() 返回 message_id / seq_id（target_id_list 仅作事实校验，不再驱动 Gateway 推送）
  ▼
Gateway
  │ 8. 把 message_id / seq_id 回响应给发送方（无状态化：不再本机推送）
  ▼
Message onDBMessage
  │ 9.  ODB 事务：写 message + user_timeline（写扩散）
  │ 10. commit 后投递 ESIndexEvent → es_index_exchange（失败落 ESOutbox）
  │ 11. commit 后投递 push 事件 → msg_push_queue（失败落 PushOutbox）
  ▼
Message onESIndexMessage         Push 服务
  仅文本写 ES `message` 索引       消费 msg_push_queue
                                   │ 12. 查 Redis 路由 → 命中本实例 WS 下发
                                   │     命中它实例 → 跨实例 brpc fanout 后 WS 下发
                                   │     失败 → CrossInstanceOutbox / PushOutbox 重投
                                   ▼
                                客户端 WS 收到 CHAT_MESSAGE_NOTIFY
```

接收端：客户端 WS 收到 `CHAT_MESSAGE_NOTIFY` 触发 UI 刷新；上线时漏收走 `GetOfflineMsg(last_message_id)` 增量补齐。

---

## 八、构建与部署

### 1. 依赖

#### 系统依赖（Linux 推荐 Ubuntu 22.04）

brpc / protobuf / odb / odb-mysql / odb-boost / hiredis / redis++ / etcd-cpp-api / elasticlient / spdlog / fmt / gtest / libcurl / libssl / cpprest / leveldb / jsoncpp / libev / amqp-cpp。

#### aws-sdk-cpp（P4 必需）

```bash
sudo bash scripts/install_aws_sdk_linux.sh
# 安装 v1.11.420 仅 S3 module 到 /usr/local，约 5–15 分钟
```

### 2. 编译

每个服务独立 CMakeLists（顶层 `CMakeLists.txt` 仅 `add_subdirectory`）：

```bash
mkdir build && cd build
cmake ..
cmake --build . -j
```

主要 target：
- `gateway_server` / `user_server` / `friend_server` / `chatsession_server`
- `transmite_server` / `message_server` / `push_server`
- `media_server` / `media_client`（原 `file_server` 重命名，P4）
- `common_tests`（公共组件单元测试聚合）

### 3. 启动基础设施

```bash
# MinIO（P4 必需）
cd docker && docker compose up -d minio minio-init && cd ..
# bucket 自动创建：chatnow-media-public / chatnow-media-private

# 顶层 docker-compose（旧，按需用：mysql / redis / es / etcd / rabbitmq）
docker compose up -d
```

### 4. 启动服务

```bash
# 例：MediaService
./build/file/media_server \
  --media_conf=conf/media.json \
  --auth_config=conf/auth.json \
  --mysql_host=127.0.0.1 --mysql_user=root --mysql_pswd=*** \
  --mysql_db=chatnow --mysql_cset=utf8mb4 \
  --redis_host=127.0.0.1 --redis_port=6379 \
  --registry_host=http://127.0.0.1:2379 \
  --listen_port=10002 --access_host=127.0.0.1:10002
```

其它服务参数模板见 `conf/<svc>_server.conf`，以 `-flagfile` 形式传入。

### 5. 烟雾测试

| 场景 | 脚本 |
|---|---|
| P2 JWT 鉴权 | `docs/operations/p2-smoke-test.md` |
| P4 媒体上传下载 | `file/test/smoke/run_smoke.sh` |

---

## 九、配置说明

| 文件 | 用途 |
|---|---|
| `conf/<svc>_server.conf` | gflags flagfile（端口 / etcd / mysql / redis / es / mq / 邮件） |
| `conf/auth.json` | JWT 多 kid（P2）|
| `conf/media.json` | s3 + media 段（双 bucket / mime 白名单 / presign） |

### 安全提示

⚠️ 仓库内的样例配置含明文密码 / 邮箱授权码 / MinIO 默认凭据，**生产部署前务必替换为安全的配置中心或环境变量**。

---

## 十、文档索引

```
docs/
├── superpowers/
│   ├── specs/                       # 设计 spec
│   └── plans/                       # 实施 plan（每个 plan 对应 1 条 feature 分支）
├── operations/                      # 运维 runbook
│   ├── jwt-key-rotation.md
│   ├── p2-smoke-test.md
│   ├── log-format-and-queries.md
│   ├── log-level-conventions.md
│   └── monitoring-and-alerting.md
├── client-sdk/                      # 客户端 SDK 契约 + 错误码重试策略
├── ARCHITECTURE_v2.0_and_roadmap.md
├── CHANGELOG_v2.0.md
└── MESSAGE_PIPELINE.md
```

### 3.0 重构进度看板（按 spec 状态盘点）

> ✅ 已实现 ｜ 🟡 部分实现（或仍在过渡） ｜ ⏳ 未开始

| Spec | 状态 | 落地证据 |
|---|---|---|
| **`2026-05-14-cross-cutting-architecture-design.md`** —— 横切基础设施总纲 | 🟡 | P1/P2/P4/P8 已落；P3 Device 模型仅表落（`odb/user_device.hxx` + `mysql_user_device.hpp`），完整多端撤销逻辑待 P3 |
| **`2026-05-14-proto-redesign.md`** —— proto 全量重构（域命名空间） | 🟡 | 9 个新域目录全部建立（`proto/identity/conversation/relationship/message/media/presence/transmite/push/common`）；老 flat proto（`base/chatsession/file/friend/message/notify/push/speech/transmite/user.proto`）仍在过渡共存 |
| **`2026-05-14-proto-redesign-gaps.md`** —— 与主流 IM 差距清单 | n/a | 评审记录，非实施项 |
| **`2026-05-14-service-migration-design.md`** —— 服务层迁移到新 proto | 🟡 | `chatsession` 已 include `conversation/conversation_service.pb.h`；`IdentityServiceImpl` 已实现；但服务类仍叫 `ChatSessionServiceImpl`（待改 `ConversationServiceImpl`），`UserService` 仍在 |
| **`2026-05-14-db-redis-changes.md`** —— DB / Redis 变更跟随 proto | ✅ | `message_reaction` / `message_pin` 表已建；`message`: `edit_time` / `forward_from_uid` / `forward_at` 已加；`chat_session_member.draft` 已加 |
| **`2026-05-13-message-reliability-hardening.md`** —— 消息可靠性加固 | ✅ | SeqGen 启动回填（`select_max_user_seq` + `backfill_session/user`）；`CrossInstanceOutbox` + reaper（`im:push:cross_outbox`）；`OnlineRoute` 惰性清理；心跳重传去耦合（commits `8c1cb6d` / `6e04346` / `d485f64` / `cf848d6`） |
| **`2026-05-13-es-dual-write-redesign.md`** —— ES 串行投递替代双写 | ✅ | `onDBMessage` DB commit 后投递 `ESIndexEvent` → `es_index_exchange` → `onESIndexMessage`；失败落 `ESOutbox` + reaper（commits `5e434ac` / `53413d3` / `6d560e8` / `90276a4`） |
| **`2026-05-13-cache-strategy-redesign.md`** —— Members / UserInfo / LastMessage 缓存 | 🟡 | `Members` 缓存已实现（warm/add/remove/invalidate）；`LastMessage` 缓存已接入；`UserInfoCache` 层尚未实现；穿透击穿防护（singleflight / null-cache）尚未实现 |
| **`2026-05-14-mq-quorum-and-seq-ordering-design.md`** —— MQ quorum + 同会话 seq 严格有序 | ⏳ | 未开工：`common/mq/` 中无 `x-queue-type=quorum` / `queue_args`；无 `consistent_hash_exchange` / `shard_index` 配置 |
| **`2026-05-14-dedup-redis-setnx.md`** —— Transmite SETNX 幂等去重 | ⏳ | 未开工：`im:dedup:` key 与 `kDedup` 常量未出现，仍走 RPC `SelectByClientMsgId` 路径 |

### 3.0 P 系列计划状态

| Plan | 状态 | 备注 |
|---|---|---|
| `2026-05-14-p1-foundations.md` —— 错误处理 / metadata / 结构化日志 | ✅ 已合并 | merge commit `138b6e6` |
| `2026-05-14-p2-jwt-multi-device-auth.md` —— JWT 多端鉴权 | ✅ 已合并 | merge commit `bbc367d` |
| `2026-05-14-p4-media-object-storage.md` —— MinIO 对象存储 | ✅ 已合并 | merge commit `dff0920` |
| `2026-05-14-p8-trace-and-structured-logs.md` —— trace_id 全链路 | ✅ 已合并 | merge commit `b73d94b` |
| P3 Device 模型（多端撤销逻辑） | 🟡 表已建，业务待补 | 仅 `user_device` 表 + DAO；JWT `device_id` 元数据已透传 |
| P5 Conversation / Message 重构 | 🟡 进行中 | proto 域已拆，服务类未改名；`ChatSessionServiceImpl` → `ConversationServiceImpl` 待执行 |
| P6 推送可靠性 + 跨实例 fanout | ✅ 大部分已落 | `CrossInstanceOutbox` + 惰性清理已合，可上线；细节优化按需 |
| P7 ASR 接入 + Profile 服务端实现 | 🟡 占位就绪 | `SpeechHandler` 占位返回空；UpdateProfile 服务端实现待补 |
| **下一阶段** —— MQ quorum + Transmite SETNX 去重 | ⏳ | 两份 spec 已就位，plan 未编 |

---

## License

仅作学习与项目演示用途。
