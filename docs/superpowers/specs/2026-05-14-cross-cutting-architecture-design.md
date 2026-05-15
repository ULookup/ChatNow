# ChatNow 横切基础设施统一架构设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-14
> **范围**: 横切基础设施（鉴权、对象存储、Device 模型、trace_id、错误处理）
> **基线**: proto/ 当前 9 个域 + `2026-05-14-proto-redesign.md` + `2026-05-14-proto-redesign-gaps.md`
> **目标**: 真实可上线的生产 IM 基线，**避免过度设计**

---

## 0. 设计原则

1. **生产可上线** — 不是 demo，所有决策按真实 IM 上线标准
2. **不过度设计** — 每个决策先问"删掉这个会怎样"，能删就删；YAGNI 优先
3. **横切优先** — 本 spec 仅定义所有服务都要遵循的横切契约，不动业务 RPC 字段（除非鉴权字段抽离这种全局变更）
4. **proto 业务体干净** — 鉴权、trace 走 transport metadata，proto 业务 Req/Rsp 体不再含 `user_id` / `session_id` / `trace_id`
5. **Gateway 是唯一鉴权点** — 后端服务在内网信任边界内只校验 metadata 存在性

明确**不做**的事：
- ❌ OpenTelemetry / Jaeger / Prometheus client（trace_id 进结构化日志够用）
- ❌ JWT 多算法支持（仅 HS256）
- ❌ 设备指纹 / IP 地理位置 / 可疑登录检测
- ❌ MinIO 生命周期 / 版本管理（仅一个 bucket family + 私有/公开两 bucket）
- ❌ 错误码 i18n 框架（客户端按 code 自映射）
- ❌ E2EE 字段预留（产品方向未定）
- ❌ 错误码 / Response 不加新元数据字段（trace_id 已在 metadata，不重复）

---

## 1. 总览架构

```
                                  ┌──────────────────────────────────┐
客户端 ──HTTP/WS──→ Gateway       │  brpc Controller Metadata 契约   │
        │              │          │  x-trace-id   : string (32 hex)  │
   Authorization:      │          │  x-user-id    : string           │
   Bearer <jwt>        │          │  x-device-id  : string           │
                       │          │  x-jwt-jti    : string           │
                       │          │  x-client-ver : string (可选)    │
                       │          └──────────────────────────────────┘
                       │
            ┌──────────┼─────────────┐ ──── brpc 内网调用全程透传
            │          │             │
       Identity   Conversation    Message    ... (其他业务服务)
       (JWT签发)  (Auth验证)      (Auth验证)
            │
            ▼
       Redis: jwt 黑名单 / refresh 反查 / refresh 重放检测
              presence 设备级 / online 路由设备级
              jwk_set (kid 多 key)

       MySQL: user_device (登录设备表)
              media_file / media_blob_ref / media_user_quota

       MinIO (对象存储)
         chatnow-media-public   (头像/群头像/sticker，public-read，CDN-ready)
         chatnow-media-private  (聊天文件/视频/语音，全 presigned URL)

       MQ (RabbitMQ) — message header 透传 trace_id
         transmite → message  (DB 落库消息)
         message   → push     (推送通知)
         message   → es       (ES 索引事件)
```

四条核心契约：

1. **proto 业务体不含鉴权 / trace 字段** — 所有 `optional string user_id` / `optional string session_id` 从所有 Req 中删除（约 50 处）
2. **Gateway 唯一面向客户端的鉴权点** — JWT 验签、查黑名单、写 metadata
3. **后端服务相互信任** — 仅校验 metadata 字段存在性，不重复验签
4. **文件传输不走 RPC** — Media 退化为"元信息 + presigned URL 签发器"

---

## 2. JWT 鉴权（生产版）

### 2.1 Token 形态

```
Access Token (HS256)
  payload: { sub: user_id, did: device_id, jti, exp, iat, kid }
  TTL: 2 小时

Refresh Token (HS256)
  payload: { sub: user_id, did: device_id, jti, exp, iat, kid, typ: "refresh" }
  TTL: 30 天
  滚动刷新：每次 RefreshToken 颁发新 access + 新 refresh，旧 refresh 立即作废 + 进重放检测表
```

`kid`（key id）固定字段，例 `"v1"`，便于密钥轮换。

### 2.2 密钥管理

- 配置含多 key map：`{ "v1": "<key>", "v2": "<key>" }`
- 配置 `auth.jwt.current_kid` 指向签发用的 key id
- 验证按 token 里的 `kid` 找密钥；若 kid 不在配置里 → `AUTH_TOKEN_INVALID`
- 密钥长度启动时 fail-fast 校验 ≥32 字节
- **密钥轮换流程（人工 runbook，不自动化）**：
  1. 加 v2 到配置，`current_kid` 仍 v1，热加载
  2. 等 30 天（>refresh TTL），确认无 v1 token
  3. 改 `current_kid=v2`，新 token 用 v2 签
  4. 30 天后从配置删除 v1

### 2.3 Redis 数据

```
im:jwt:revoked:{jti}                  →  "1"     TTL = token 剩余寿命
im:jwt:rt:{user_id}:{device_id}       →  refresh_jti
im:jwt:rt_chain:{old_jti}             →  "rotated"  TTL = 24h    (重放检测)
```

**重放检测**：RefreshToken 时若发现传入的 refresh_jti 出现在 `rt_chain` 中 → 该 device 已被攻击者用过旧 refresh → 立即吊销该 device 所有 token，返回 `AUTH_REFRESH_TOKEN_REUSED`。

### 2.4 IdentityService RPC（认证部分）

| RPC | 入参 | 响应 |
|---|---|---|
| `Register` | 已有（oneof credential） | `RegisterRsp { header, user_id }` |
| `Login` | 已有（oneof credential） + device 字段（见 §4.3） | `LoginRsp { header, AuthTokens, UserInfo }` |
| `Logout` | `LogoutReq { request_id }`（user_id/device_id 从 metadata 取） | `LogoutRsp { header }` — 吊销该设备所有 token |
| `RefreshToken` | `RefreshTokenReq { request_id, refresh_token }` | `RefreshTokenRsp { header, AuthTokens }` |
| `SendVerifyCode` | 已有 | 已有 |

```protobuf
message AuthTokens {
    string access_token = 1;
    string refresh_token = 2;
    int32  access_expires_in_sec = 3;
    int32  refresh_expires_in_sec = 4;
}
```

### 2.5 Gateway 验签流程

```
1. 取 HTTP header: Authorization: Bearer <token>
   缺失 → 401 + AUTH_TOKEN_INVALID
2. 解析 token 头部 kid → 取对应密钥
   kid 不在配置 → 401 + AUTH_TOKEN_INVALID
3. HS256 验签
   验签失败 → 401 + AUTH_TOKEN_INVALID
4. 检查 exp < now
   超期 → 401 + AUTH_TOKEN_EXPIRED
5. 查 Redis im:jwt:revoked:{jti}
   命中 → 401 + AUTH_TOKEN_INVALID
6. 解析 payload → 写 brpc Controller metadata：
     x-user-id, x-device-id, x-jwt-jti, x-trace-id
7. 调下游 RPC
```

**例外路径**（不需要 JWT）：`Login` / `Register` / `SendVerifyCode` / `RefreshToken` — Gateway 直接放行，仅生成 trace_id 写 metadata。

### 2.6 后端服务统一入口

```cpp
// common/auth/auth_context.hpp
struct AuthContext {
    std::string user_id;
    std::string device_id;
    std::string trace_id;
};

// 强校验：metadata 缺字段 → throw ServiceError(SYSTEM_INTERNAL_ERROR)
//        说明 Gateway 没正确填或调用方未透传
AuthContext extract_auth(brpc::Controller* cntl);
```

每个 handler 入口一行 `auto auth = extract_auth(cntl);`。后端**不再做 JWT 验签**。

### 2.7 服务间内部调用

调用方必须透传入站 metadata：

```cpp
// common/auth/forward_auth.hpp
void forward_auth_metadata(brpc::Controller* in_cntl, brpc::Controller* out_cntl);
// 复制 x-trace-id, x-user-id, x-device-id, x-jwt-jti 四个字段
```

**例外**：内部清理 worker / 定时任务等"无 user 上下文"的内部调用，metadata 中 `x-user-id` 填特殊值 `__system__`，`x-device-id` 填 `__internal__`。被调服务在权限检查时识别此特殊值放行（仅限可信内部 RPC）。

### 2.8 proto 字段清理

所有业务 Req 删除：
```
optional string user_id = N;
optional string session_id = N;
```

波及文件（约 50 处字段）：
- `conversation_service.proto` 17 个 RPC 全部
- `relationship_service.proto` 9 个 RPC
- `identity_service.proto` GetProfile/UpdateProfile/SearchUsers
- `media_service.proto` 全部
- `transmite_service.proto` NewMessageReq

---

## 3. Media 对象存储（生产版）

### 3.1 上传两种模式

| 模式 | 适用 | RPC |
|---|---|---|
| **单段** | ≤100MB | ApplyUpload → 客户端 PUT → CompleteUpload |
| **分片**（multipart） | >100MB（视频、大附件） | InitMultipartUpload → ApplyPartUpload×N → CompleteMultipartUpload |

```protobuf
service MediaService {
    rpc ApplyUpload(ApplyUploadReq) returns (ApplyUploadRsp);
    rpc CompleteUpload(CompleteUploadReq) returns (CompleteUploadRsp);

    rpc InitMultipartUpload(InitMultipartReq) returns (InitMultipartRsp);
    rpc ApplyPartUpload(ApplyPartReq) returns (ApplyPartRsp);
    rpc CompleteMultipartUpload(CompleteMultipartReq) returns (CompleteMultipartRsp);
    rpc AbortMultipartUpload(AbortMultipartReq) returns (AbortMultipartRsp);

    rpc ApplyDownload(ApplyDownloadReq) returns (ApplyDownloadRsp);
    rpc GetFileInfo(GetFileInfoReq) returns (GetFileInfoRsp);

    rpc SpeechRecognition(SpeechRecognitionReq) returns (SpeechRecognitionRsp);
}

message ApplyUploadReq {
    string request_id = 1;
    string file_name = 2;
    int64  file_size = 3;
    string mime_type = 4;
    string content_hash = 5;            // "sha256:hex" 客户端预算
    MediaPurpose purpose = 6;            // AVATAR / GROUP_AVATAR / CHAT / STICKER
}
message ApplyUploadRsp {
    ResponseHeader header = 1;
    string file_id = 2;
    bool   already_exists = 3;           // true: 直接 CompleteUpload 即可
    string upload_url = 4;
    map<string, string> headers = 5;     // PUT 时必须携带（Content-Type 等）
    int32  expires_in_sec = 6;
}

message CompleteUploadReq { string request_id = 1; string file_id = 2; }
message CompleteUploadRsp { ResponseHeader header = 1; FileInfo file_info = 2; }

message InitMultipartRsp {
    ResponseHeader header = 1;
    string file_id = 2;
    string upload_id = 3;
    int32  recommended_part_size_bytes = 4;   // 默认 8MB
}
message ApplyPartReq {
    string request_id = 1;
    string upload_id = 2;
    int32  part_number = 3;              // 1..10000
}
message ApplyPartRsp {
    ResponseHeader header = 1;
    string upload_url = 2;
    int32  expires_in_sec = 3;
}
message CompleteMultipartReq {
    string request_id = 1;
    string upload_id = 2;
    repeated PartETag parts = 3;
}
message PartETag { int32 part_number = 1; string etag = 2; }

message ApplyDownloadReq { string request_id = 1; string file_id = 2; }
message ApplyDownloadRsp {
    ResponseHeader header = 1;
    string download_url = 2;
    int32  expires_in_sec = 3;
    FileInfo file_info = 4;
}

message FileInfo {
    string file_id = 1;
    string file_name = 2;
    int64  file_size = 3;
    string mime_type = 4;
    int64  uploaded_at_ms = 5;
}

enum MediaPurpose {
    MEDIA_PURPOSE_UNSPECIFIED = 0;
    AVATAR = 1;
    GROUP_AVATAR = 2;
    CHAT = 3;
    STICKER = 4;
}
```

废弃旧的 `UploadFile / DownloadFile / FileUploadData / FileDownloadData`（含 bytes 字段全部删除）。

### 3.2 Bucket 与对象 Key

```
chatnow-media-public    bucket policy: public-read（CDN 前置友好）
  prefix: avatar/        头像
  prefix: group_avatar/  群头像
  prefix: sticker/       表情包

chatnow-media-private   全部 presigned URL 访问
  prefix: chat/{date}/   聊天图片/视频/文件/语音

object key: {prefix}{date}/{content_hash[0:2]}/{content_hash}
  例: chat/2026/05/14/a1/a1b2c3d4...
```

按日期 + hash 前缀双层分片，避免单 prefix object 数过大（MinIO 单 prefix >1M 性能下降）。

**bucket 路由**：服务端按 `MediaPurpose` 决定 bucket，客户端无法干预。

### 3.3 元信息表（MySQL）

```sql
CREATE TABLE media_file (
  file_id        VARCHAR(32) PRIMARY KEY,
  content_hash   VARCHAR(72) NOT NULL,         -- "sha256:hex"
  bucket         VARCHAR(64) NOT NULL,
  object_key     VARCHAR(255) NOT NULL,
  file_name      VARCHAR(255),
  file_size      BIGINT NOT NULL,
  mime_type      VARCHAR(128) NOT NULL,
  purpose        TINYINT NOT NULL,             -- MediaPurpose enum
  owner_id       VARCHAR(32) NOT NULL,
  uploaded_at_ms BIGINT NOT NULL,
  status         TINYINT NOT NULL,             -- 0=pending,1=committed,2=deleted,3=quarantined
  upload_id      VARCHAR(128),                  -- multipart only
  KEY idx_hash (content_hash),
  KEY idx_owner (owner_id, uploaded_at_ms),
  KEY idx_status_uploaded (status, uploaded_at_ms)
);

CREATE TABLE media_blob_ref (
  content_hash VARCHAR(72) NOT NULL,
  bucket       VARCHAR(64) NOT NULL,
  object_key   VARCHAR(255) NOT NULL,
  ref_count    INT NOT NULL DEFAULT 0,
  total_size   BIGINT NOT NULL,
  last_decremented_at_ms BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (content_hash)
);

CREATE TABLE media_user_quota (
  user_id        VARCHAR(32) PRIMARY KEY,
  used_bytes     BIGINT NOT NULL DEFAULT 0,
  quota_bytes    BIGINT NOT NULL DEFAULT 5368709120,  -- 5GB
  updated_at_ms  BIGINT NOT NULL
);
```

**status 流转**：
- `0 pending` → ApplyUpload 写入
- `1 committed` → CompleteUpload 后切换；ref_count++
- `2 deleted` → ref_count=0 且 7 天 GC 缓冲后物理删除
- `3 quarantined` → magic number 嗅探不符 / 安全策略命中

### 3.4 服务端强制校验

| 校验项 | 时机 | 失败动作 |
|---|---|---|
| mime 白名单 | ApplyUpload | `MEDIA_UNSUPPORTED_FORMAT` |
| size 上限（按 mime 分级） | ApplyUpload | `MEDIA_FILE_TOO_LARGE` |
| 配额 | ApplyUpload | `MEDIA_QUOTA_EXCEEDED` |
| 真实 size 比对 | CompleteUpload (HeadObject) | `MEDIA_HASH_MISMATCH` + 删 MinIO 对象 |
| 真实 etag 比对 | CompleteUpload | `MEDIA_HASH_MISMATCH` 同上 |
| magic number 嗅探 | CompleteUpload 后异步 worker | status=3 quarantined，用户不可下载 |

mime 白名单（配置文件，hot reload）：
```
image/jpeg image/png image/gif image/webp        max=20MB
video/mp4 video/webm video/quicktime             max=500MB（强制 multipart）
audio/aac audio/mp4 audio/ogg audio/webm         max=50MB
application/pdf                                  max=100MB
text/plain                                       max=5MB
其他                                              拒绝
```

magic number v1 嗅探最小集（~50 行）：
- 拒绝 PE (`MZ`)、ELF (`\x7fELF`)、Mach-O (`\xfe\xed\xfa\xce` / `\xce\xfa\xed\xfe` 等)
- 检测 `image/*` 是否真为图片 magic（JPEG `FFD8FF`、PNG `89504E47`、GIF `474946`、WebP `RIFF...WEBP`）
- 不符 → `status=3 quarantined`

未来需要更全检测可换 libmagic。

### 3.5 用户配额

- 默认 5GB（配置可调）
- ApplyUpload 时：检查 `used_bytes + file_size + Σ(其他 pending) <= quota_bytes`，否则 `MEDIA_QUOTA_EXCEEDED`
- CompleteUpload 时：`used_bytes += file_size`
- 文件物理删除（GC 时）：`used_bytes -= file_size`

### 3.6 自动清理 worker（生产必备）

**MediaService 启动时拉起一个内部线程**，不依赖外部 cron：

| 任务 | 周期 | 动作 |
|---|---|---|
| pending 超时清理 | 每 5 分钟 | `WHERE status=0 AND uploaded_at_ms < now-1h` → 若 ref_count 仅此条则删 MinIO 对象 + 删表行 |
| multipart 孤儿清理 | 每 1 小时 | 调 MinIO `ListMultipartUploads`，>24h 未完成 → AbortMultipartUpload |
| 未引用 blob GC | 每天凌晨 3:00 | `WHERE ref_count=0 AND last_decremented_at_ms < now-7d` → 删 MinIO 对象 + 删 ref 行 + 减用户配额 |
| quarantined 清理 | 每天凌晨 4:00 | `WHERE status=3 AND uploaded_at_ms < now-7d` → 删对象 + 标记 deleted |

**7 天 GC 缓冲期**：用户删除消息 / 头像 / 群解散触发 ref_count--，但 7 天后才物理删，留撤销窗口（参照 Telegram）。

**worker 加分布式锁**（防多实例同时跑）：
```
redis SET im:media:gc:lease  instance_id  EX 600 NX
```

### 3.7 调用方改造

旧：`MediaService.UploadFile(bytes)` → 改为客户端走 ApplyUpload / 直传 / CompleteUpload，拿到 file_id。

| 调用方 | 旧行为 | 新行为 |
|---|---|---|
| Identity | 上传头像 bytes | 客户端先上传到 MinIO，调 `UpdateProfile(avatar_file_id="xxx")` |
| Conversation | 上传群头像 bytes | 同上，`UpdateConversation(avatar_file_id="xxx")` |
| Message | 拉取附件 bytes | 客户端按 ImageContent.file_id 调 `ApplyDownload` 直接 GET |

**Avatar 特殊路径**：`UserInfo.avatar_url` 仍然是 URL 字符串（不是 file_id），原因是好友列表批量返回 100 个 UserInfo 不能让客户端再批 100 次 ApplyDownload。

实现方式：avatar 存 public bucket，`UserInfo.avatar_url = <media.public_url_prefix>/avatar/{file_id}`。public bucket policy 设为 public-read，URL 永久可访问，CDN 可前置。

`UpdateProfile` 入参字段：
```protobuf
message UpdateProfileReq {
    string request_id = 1;
    optional string nickname = 4;
    optional string bio = 5;
    optional string avatar_file_id = 6;   // 改为 file_id；服务端转换为 URL 写库
    optional string phone = 7;
}
```

`Conversation.avatar_url`、`StickerContent.sticker_id`（sticker 在 public bucket）同样路径。

### 3.8 SpeechRecognition

短音频 ≤2MB ≤30s 仍 RPC + bytes（直接喂 ASR 模型）。长音频走 file_id（v1 不实施）。

### 3.9 SDK 选择

使用 **aws-sdk-cpp 的 S3 module**（only S3，不带其他 module）。理由：
- S3 V4 签名算法过去几年有 minor 修订，自实现需持续跟进
- 签名 bug 是静默错误，最难排查
- aws-sdk-cpp battle-tested

依赖加入 `depends.sh` 与 `CMakeLists.txt`。

### 3.10 MinIO 部署

```yaml
# docker-compose.yml 新增
minio:
  image: minio/minio:RELEASE.2024-xx-xx
  command: server /data --console-address ":9001"
  environment:
    MINIO_ROOT_USER: <配置注入>
    MINIO_ROOT_PASSWORD: <配置注入>
  volumes:
    - minio-data:/data
  ports:
    - "9000:9000"
    - "9001:9001"
```

启动后 entrypoint.sh 用 `mc` 创建 bucket：
```bash
mc alias set local http://minio:9000 $ROOT_USER $ROOT_PWD
mc mb -p local/chatnow-media-public
mc mb -p local/chatnow-media-private
mc anonymous set download local/chatnow-media-public  # public-read
```

### 3.11 监控埋点

结构化日志（不引 Prometheus）：
- 每次 ApplyUpload/CompleteUpload/ApplyDownload 一行：`{trace_id, user_id, file_id, size, mime_type, action, latency_ms}`
- 清理 worker 每轮一行：`{cleaned_pending, aborted_multipart, gc_blob, gc_total_size}`
- ERROR 级日志：配额拒绝 / mime 拒绝 / size 超限 / hash 不符

---

## 4. Device 模型 + Presence 设备级 + Push 设备级路由

### 4.1 Device 实体（新增到 common/types.proto）

```protobuf
enum DevicePlatform {
    DEVICE_PLATFORM_UNSPECIFIED = 0;
    IOS = 1;
    ANDROID = 2;
    WEB = 3;
    DESKTOP_WIN = 4;
    DESKTOP_MAC = 5;
    DESKTOP_LINUX = 6;
}

message Device {
    string device_id = 1;            // 客户端首次安装生成（UUID v4），本地持久化
    DevicePlatform platform = 2;
    string device_name = 3;          // "iPhone 15 / Chrome on macOS"
    string app_version = 4;
    string os_version = 5;
    int64  first_login_at_ms = 6;
    int64  last_active_at_ms = 7;
    string last_login_ip = 8;
    bool   is_current = 9;           // ListDevices 响应里标记当前请求的设备
}
```

### 4.2 user_device 表

```sql
CREATE TABLE user_device (
  user_id           VARCHAR(32) NOT NULL,
  device_id         VARCHAR(64) NOT NULL,
  platform          TINYINT NOT NULL,
  device_name       VARCHAR(128),
  app_version       VARCHAR(32),
  os_version        VARCHAR(32),
  first_login_at_ms BIGINT NOT NULL,
  last_active_at_ms BIGINT NOT NULL,
  last_login_ip     VARCHAR(64),
  status            TINYINT NOT NULL,    -- 0=active, 1=revoked
  PRIMARY KEY (user_id, device_id),
  KEY idx_last_active (user_id, last_active_at_ms)
);
```

### 4.3 Login 流程整合

```protobuf
message LoginReq {
    string request_id = 1;
    oneof credential {
        UsernamePassword username_pwd = 2;
        PhoneVerifyCode phone_code = 3;
    }
    string device_id = 4;            // 客户端生成
    DevicePlatform platform = 5;
    string device_name = 6;
    string app_version = 7;
    string os_version = 8;
}
```

服务端流程：
1. 凭据校验
2. 查 `user_device WHERE user_id=$1 AND platform=$2 AND status=active` 列表
3. 若 `count >= auth.device_concurrency_per_platform`（默认 1）→ 选最旧的踢掉：写黑名单 + 推 `KICKED_BY_NEW_DEVICE` 通知
4. UPSERT user_device（按 PK 存在则更新 last_active 等）
5. 颁发 JWT（payload 含 device_id）
6. 返回 AuthTokens + UserInfo

**多端共存策略**：每 platform 同时 1 个活跃设备。PC/Mac/iOS/Android 可共存，2 个 iOS 不行。配置项 `auth.device_concurrency_per_platform` 可调。

### 4.4 IdentityService Device RPC 新增

```protobuf
service IdentityService {
    // ... 已有 RPC
    rpc ListDevices(ListDevicesReq) returns (ListDevicesRsp);
    rpc RevokeDevice(RevokeDeviceReq) returns (RevokeDeviceRsp);
    rpc RenameDevice(RenameDeviceReq) returns (RenameDeviceRsp);
}

message ListDevicesReq { string request_id = 1; }
message ListDevicesRsp { ResponseHeader header = 1; repeated Device devices = 2; }

message RevokeDeviceReq { string request_id = 1; string target_device_id = 2; }
message RevokeDeviceRsp { ResponseHeader header = 1; }

message RenameDeviceReq {
    string request_id = 1;
    string target_device_id = 2;
    string new_device_name = 3;
}
message RenameDeviceRsp { ResponseHeader header = 1; }
```

`RevokeDevice` 实现：
1. `target_device_id == auth.device_id` → 返回 `DEVICE_REVOKE_SELF`（自己用 Logout）
2. 查 `im:jwt:rt:{user_id}:{target_device_id}` → 拿到 refresh jti
3. 写 `im:jwt:revoked:{jti}`、删反查表
4. `UPDATE user_device SET status=1 WHERE ...`
5. 通知 Push 服务断开该设备 WS 连接（通过 cross-instance 路由）
6. 推送 `KICKED_BY_REVOKE` 通知给被踢设备

### 4.5 Presence 设备级（修订 presence/presence_service.proto）

```protobuf
message DevicePresence {
    string device_id = 1;
    DevicePlatform platform = 2;
    PresenceState state = 3;
    int64  last_active_at_ms = 4;
}

message Presence {
    string user_id = 1;
    PresenceState aggregated_state = 2;       // 多设备聚合
    int64  last_active_at_ms = 3;             // 所有设备 max
    repeated DevicePresence devices = 4;
    optional string custom_status = 5;
}

message SetPresenceReq {
    string request_id = 1;
    PresenceState state = 2;
    optional string custom_status = 3;
    // user_id, device_id 从 metadata 取
}
```

**aggregated_state 计算**：取所有 device 的 max（ONLINE > BUSY > AWAY > INVISIBLE > OFFLINE）。
**INVISIBLE 行为**：他人看到 OFFLINE，自己看到真实值（device 列表中 INVISIBLE 不下发给他人）。

### 4.6 Presence Redis（设备级）

```
im:presence:device:{user_id}:{device_id}  HASH { state, platform, last_active_at_ms }  TTL 120s
im:presence:user:{user_id}                 SET 包含 device_id 列表
im:presence:typing:{user_id}               SET conversation_id  TTL 10s
im:presence:sub:{user_id}                  SET subscriber_user_ids
```

**心跳**：Push 进程收到 WS 心跳 → 直接更新 `im:presence:device:{u}:{d}` last_active_at_ms 并 EXPIRE 120s。无 RPC 开销。

**状态变化推送**：device 状态变化时（上线/离线/INVISIBLE 切换），由 Presence 模块向 `im:presence:sub:{user_id}` 中的订阅者推 `NotifyPresenceChange`。

### 4.7 Push 设备级路由

WS 连接 key 从 `user_id` 改为 `(user_id, device_id)`。

Redis 路由表：
```
im:online:{user_id}                     HASH { device_id_1: instance_1, device_id_2: instance_2 }
im:online:device:{user_id}:{device_id}  STRING instance_id  TTL 120s（心跳续期）
im:push:unacked:{user_id}:{device_id}   ZSET 未 ACK 的消息（按 user_seq 排序）
```

**PushService 内部逻辑**：
- 推消息按 user_id 查所有 device → 每个 device 路由到对应 instance → 按 instance 聚合后跨实例转发（已有的 PushBatch）
- ACK 按 (user_id, device_id, message_id) 维度，避免 A 设备 ACK 了 B 设备就漏推
- UnackedPush 按设备维度独立缓冲

```protobuf
message PushToUserReq {
    string request_id = 1;
    string user_id = 2;
    NotifyMessage notify = 3;                // 强类型（不再 bytes，符合 §6 修订）
    optional uint64 user_seq = 4;
    repeated string target_device_ids = 5;   // 空=所有设备；非空=仅指定设备（如 KICKED 仅推被踢设备）
}

message NotifyMsgPushAck {
    string user_id = 1;
    string device_id = 2;                    // 新增
    int64  message_id = 3;
    uint64 user_seq = 4;
    string conversation_id = 5;
}
```

### 4.8 user_seq 仍按用户全局（不按设备）

设备级是"路由维度"和"在线维度"，**消息已读状态仍是用户维度**。
- 一份 user_seq，所有设备共享
- 已读 `(user_id, conversation_id, last_read_seq)` 一份
- 这是行业默认（微信/Telegram/Slack）

### 4.9 KICKED 通知

```protobuf
enum NotifyType {
    // ... 已有
    KICKED_BY_NEW_DEVICE = 8;
    KICKED_BY_REVOKE = 9;
    FORCE_LOGOUT = 10;
}

message NotifyKicked {
    string reason = 1;            // "new_device_login" / "manual_revoke" / "security_policy"
    string new_device_name = 2;   // 仅 reason=new_device_login 时有
    int64  kicked_at_ms = 3;
}
```

被踢客户端收到 → 清空本地 token + 跳登录页。

### 4.10 影响清单

| 服务 | 改动 |
|---|---|
| Identity | LoginReq 加 device 字段；UPSERT user_device；新增 ListDevices/RevokeDevice/RenameDevice；JWT payload 含 device_id；多端策略检查 |
| Push | WS 路由 key 改 (user, device)；UnackedPush 改设备级；ACK 处理改设备级；KICKED 推送处理 |
| Presence | RPC 入参/响应改设备级；Redis key 改设备级；心跳处理改设备级；aggregated_state 计算 |
| Conversation/Message/Relationship | 不直接改设备级；客户端按设备处理 |

---

## 5. trace_id + 日志 + 错误处理（生产版）

### 5.1 trace_id 生成与透传

**生成点**：Gateway 唯一生成。
- 客户端送 `X-Trace-Id` header 且格式合法 → 透传
- 否则 Gateway 生成（snowflake 复用 worker_id 池）
- 格式：16 字节 hex（32 字符），与未来 OpenTelemetry W3C trace-id 兼容

**透传链路**：
```
HTTP Header  X-Trace-Id        →  Gateway
brpc metadata  x-trace-id       →  所有后端 RPC（含服务间内部调用）
MQ message header  trace_id     →  RabbitMQ headers 字段
WS frame       payload 字段     →  推给客户端，便于客户端 SDK 关联
```

**MQ trace_id 透传是生产必备**：客户端发消息 → Gateway → Transmite → MQ → Message 落库 → MQ → Push → WS，整条链路可按 trace_id grep。

### 5.2 结构化日志（spdlog JSON）

```json
{
  "ts": "2026-05-14T10:23:45.123Z",
  "level": "INFO",
  "service": "message",
  "trace_id": "a1b2c3...",
  "user_id": "u_123",
  "device_id": "d_xyz",
  "msg": "consumed_db_message",
  "fields": { "message_id": 9876, "conversation_id": "c_001", "latency_ms": 12 }
}
```

**日志级别约定**：

| 级别 | 用法 |
|---|---|
| ERROR | 影响业务结果的失败（DB/MQ/外部服务失败） |
| WARN | 客户端错误（鉴权失败、参数非法、配额超限）；降级触发 |
| INFO | RPC 入口/出口（仅 Gateway 入口 + 服务边界）；MQ 消费成功 |
| DEBUG | 默认关闭，本地调试 |

**禁止**：散落的 `LOG_INFO("step 1")` 调试 INFO；`LOG_ERROR(rsp.errmsg())` 缺上下文的日志。

**LogContext (MDC)**：
```cpp
// common/log/log_context.hpp
class LogContext {
public:
    static void set(const std::string& trace_id, const std::string& user_id, const std::string& device_id);
    static void clear();
};
```
- thread_local 存储
- LOG_xxx 宏自动从 MDC 读取并写入日志，业务代码只写 msg + fields

### 5.3 ResponseHeader 规范

字段保持现有：`request_id, success, error_code, error_message`。**不加 trace_id 字段**（已在 metadata，不重复）。

约束：
- `error_code = OK(0)` ⇔ `success = true`
- `success = false` ⇒ `error_code` 非 0 且使用 `ErrorCode` 枚举值
- `error_message` **生产客户端 UI 不直接展示**，按 error_code 自映射文案
- `request_id` 调用方填，服务端原样回填

### 5.4 ErrorCode 全集

```protobuf
enum ErrorCode {
    OK = 0;

    // 1000-1999 认证
    AUTH_INVALID_CREDENTIALS = 1001;
    AUTH_TOKEN_EXPIRED       = 1002;
    AUTH_TOKEN_INVALID       = 1003;
    AUTH_USER_NOT_FOUND      = 1004;
    AUTH_USER_ALREADY_EXISTS = 1005;
    AUTH_VERIFY_CODE_INVALID = 1006;
    AUTH_VERIFY_CODE_EXPIRED = 1007;
    AUTH_REFRESH_TOKEN_REUSED = 1008;     // 重放检测
    AUTH_DEVICE_REVOKED       = 1009;     // user_device.status=revoked

    // 2000-2999 关系
    RELATIONSHIP_ALREADY_FRIENDS = 2001;
    RELATIONSHIP_NOT_FRIENDS     = 2002;
    RELATIONSHIP_BLOCKED         = 2003;
    RELATIONSHIP_REQUEST_PENDING = 2004;

    // 3000-3999 会话
    CONVERSATION_NOT_FOUND     = 3001;
    CONVERSATION_NOT_MEMBER    = 3002;
    CONVERSATION_NO_PERMISSION = 3003;
    CONVERSATION_MEMBER_LIMIT  = 3004;

    // 4000-4999 消息
    MESSAGE_NOT_FOUND        = 4001;
    MESSAGE_RECALL_TIMEOUT   = 4002;
    MESSAGE_ALREADY_RECALLED = 4003;
    MESSAGE_CONTENT_INVALID  = 4004;

    // 5000-5999 媒体
    MEDIA_FILE_TOO_LARGE     = 5001;
    MEDIA_UNSUPPORTED_FORMAT = 5002;
    MEDIA_UPLOAD_FAILED      = 5003;
    MEDIA_QUOTA_EXCEEDED     = 5004;
    MEDIA_HASH_MISMATCH      = 5005;
    MEDIA_UPLOAD_INCOMPLETE  = 5006;
    MEDIA_PART_NOT_FOUND     = 5007;

    // 6000-6999 Presence
    PRESENCE_USER_OFFLINE = 6001;

    // 7000-7999 Device
    DEVICE_NOT_FOUND      = 7001;
    DEVICE_REVOKE_SELF    = 7002;
    DEVICE_LIMIT_EXCEEDED = 7003;

    // 8000-8999 限流
    RATE_LIMIT_EXCEEDED = 8001;

    // 9000-9999 系统
    SYSTEM_INTERNAL_ERROR    = 9001;
    SYSTEM_UNAVAILABLE       = 9002;
    SYSTEM_TIMEOUT           = 9003;
    SYSTEM_INVALID_ARGUMENT  = 9004;
}
```

### 5.5 ServiceError + HANDLE_RPC 宏

```cpp
// common/error/service_error.hpp
class ServiceError : public std::exception {
public:
    ServiceError(ErrorCode code, std::string msg);
    ErrorCode code() const;
    const std::string& message() const;
};
```

业务代码：
```cpp
throw ServiceError(ErrorCode::CONVERSATION_NOT_MEMBER, "user_id=u_123 not in conv=c_001");
```

每个 RPC handler 包裹宏：
```cpp
HANDLE_RPC(req, rsp, {
    auto auth = extract_auth(cntl);  // 已在宏内部
    // 业务逻辑，仅 throw ServiceError，不手填 ResponseHeader
});
```

宏的标准实现：
```cpp
#define HANDLE_RPC(req, rsp, body) \
    auto _auth = extract_auth(cntl); \
    LogContext::set(_auth.trace_id, _auth.user_id, _auth.device_id); \
    try { \
        auto& auth = _auth; (void)auth; \
        body \
        rsp->mutable_header()->set_success(true); \
        rsp->mutable_header()->set_error_code(ErrorCode::OK); \
        rsp->mutable_header()->set_request_id(req->request_id()); \
    } catch (const ServiceError& e) { \
        rsp->mutable_header()->set_success(false); \
        rsp->mutable_header()->set_error_code(e.code()); \
        rsp->mutable_header()->set_error_message(e.message()); \
        rsp->mutable_header()->set_request_id(req->request_id()); \
        LOG_WARN("rpc_failed", "code", e.code(), "msg", e.message()); \
    } catch (const std::exception& e) { \
        rsp->mutable_header()->set_success(false); \
        rsp->mutable_header()->set_error_code(ErrorCode::SYSTEM_INTERNAL_ERROR); \
        rsp->mutable_header()->set_error_message("internal error"); \
        rsp->mutable_header()->set_request_id(req->request_id()); \
        LOG_ERROR("rpc_exception", "what", e.what()); \
    } \
    LogContext::clear();
```

### 5.6 内部错误不泄漏

- `ServiceError` → `error_message` 原样返回（业务可读，已经被开发者审查过）
- 其他 `std::exception` → `error_message` 强制为 `"internal error"`，原 `what()` 仅写日志
- 永远不暴露 SQL 错误、栈信息、内部 IP、文件路径

### 5.7 客户端重试策略约定

| ErrorCode 段 | 客户端动作 |
|---|---|
| 1xxx 认证 | 不重试；TOKEN_EXPIRED → 自动 RefreshToken 后重试 1 次；其他跳登录页 |
| 2xxx-7xxx 业务 | 不重试；UI 提示按 code 映射 |
| 8001 限流 | 退避重试（指数退避 + 抖动，最多 3 次） |
| 9002/9003 系统不可用/超时 | 退避重试；连续 3 次失败 → 提示"服务暂时不可用" |
| 9001 内部错误 | 不重试 |

客户端 SDK 必须按此实现。

### 5.8 监控告警接入

ERROR 级日志即告警源。运维侧用 ELK/Loki + 日志告警规则（不在本 spec 实施，但日志格式必须支持检索）：
- ERROR 1 分钟内 >100 条 → 告警
- 9001/9002 占比 > 1% → 告警
- 3 分钟无 INFO → 服务可能挂

---

## 6. 与已有 spec 的衔接

本 spec 是横切基础设施定义。以下 gaps 项已被本 spec 覆盖：

| Gap | 处置 |
|---|---|
| G1 鉴权字段抽离 metadata | §2.6 / §2.7 / §2.8 已定义 |
| G2 文件预签名 URL | §3 已完整定义 |
| G3 Device 模型 | §4 已完整定义 |
| G5 PushService 强类型 NotifyMessage | §4.7 修订 PushToUserReq.notify 为强类型 |
| G10 Token 三件套 | §2.1 / §2.4 已定义 AuthTokens |

未在本 spec 处理（留待后续 spec）：
- G4 ListConversations 增量同步 token
- G6 NotifyType 业务/控制帧拆分
- G7 Reactions 变更 notify
- G8 CreateConversation 缺 type
- G9 MarkRead 双轨
- G11 UserSeqPair 重复定义
- P1-P5 产品决策类

---

## 7. 实施顺序建议

| 顺序 | 模块 | 理由 |
|---|---|---|
| 1 | common 层（auth / error / log_context） | 所有服务依赖 |
| 2 | proto 修订（删 user_id/session_id；加 AuthTokens / Device / Media 新 RPC / NotifyMessage 强类型 / ErrorCode 7xxx） | 服务实现的契约 |
| 3 | Gateway JWT 验签 + metadata 写入 | 入口 |
| 4 | Identity（JWT 签发、Device 表、Login/Logout/Refresh、ListDevices/RevokeDevice）| 鉴权后端 |
| 5 | MinIO 部署 + Media 服务重写（ApplyUpload / 三步上传 / multipart / 清理 worker） | 文件能力 |
| 6 | Presence 设备级改造（Redis key + RPC 入参） | Push 上游 |
| 7 | Push 设备级路由（WS key / UnackedPush / ACK） | 推送 |
| 8 | 其他业务服务（Conversation/Message/Relationship/Transmite）按 §2.6 / §5.5 套用 HANDLE_RPC 宏，删除 user_id/session_id 字段读取 | 收尾 |

---

## 8. 验收标准

代码验收（每项需测试覆盖）：
1. 任何 proto Req/Rsp 不再包含 `user_id` / `session_id` 字段（grep 确认）
2. 所有后端服务 RPC handler 入口使用 `HANDLE_RPC` 宏 / 等价封装
3. JWT 滚动刷新 + 重放检测路径有单元测试
4. Media ApplyUpload → CompleteUpload → ApplyDownload 端到端测试
5. Media 自动清理 worker 4 类任务有集成测试
6. Login 触发同 platform 旧设备踢出有集成测试
7. Push 设备级 ACK 不串扰有集成测试
8. trace_id 端到端透传（Gateway → Transmite → MQ → Message → MQ → Push → WS）日志可关联

文档验收：
- 密钥轮换 runbook 写入 `docs/operations/jwt-key-rotation.md`
- MinIO bucket 创建 / 备份策略写入 `docs/operations/minio-setup.md`

---

## 9. 后续 spec

本 spec 完成后，建议依次开下面的 spec（各自独立，可并行）：

1. **proto-redesign-v2**：处理 G4/G6/G7/G8/G9/G11
2. **product-decisions**：处理 P1-P5（thread / E2EE / CHANNEL / Bot / i18n）
3. **service-migration-v2**：基于本 spec 后的 proto 重新规划服务层迁移（替代 `2026-05-14-service-migration-design.md`）
