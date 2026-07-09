# ChatNow E2E 测试设计

> **状态**: 设计完成，待评审
> **日期**: 2026-07-08
> **范围**: L3 端到端场景测试套件专项设计
> **定位**: 跨服务 E2E 链路 + 数据一致性断言 + 实时推送验证

---

## 0. E2E 定义

### 0.1 什么是 E2E 测试

E2E（End-to-End）测试是**跨多服务、多 API 串联**的真实用户旅程验证，回答一个问题：

> **"真实用户按预期路径操作时，多个服务协作的最终行为是否正确？"**

E2E 不验证单个 API 的入参出参（那是 L2 的职责），而是验证**多个 API 串联后的链路正确性**，包括：
- 跨服务数据一致性（发消息后 DB/ES/MinIO 实际落库）
- 实时推送正确性（WS 通知送达且内容匹配）
- 跨设备状态同步（多端登录、未读数同步）
- 故障恢复正确性（MQ 断连重连、WS 断线重连补齐）

### 0.2 E2E 在测试金字塔中的位置

```
L4  Performance       nightly           基准 + 回归阈值
L3  E2E Scenario      PR to 3.0-dev     跨服务链路 + 数据一致性  ← 本文档
L2  Functional        每 PR             每服务 API + 错误路径
L1  BVT               每次构建          烟雾测试，核心链路冒烟
L0  Build             每次 push         编译 + 静态检查
```

### 0.3 E2E vs L2 Functional 的区别

| 维度 | E2E Scenario | L2 Functional |
|---|---|---|
| 目的 | 验证多服务协作链路 | 验证单 API 行为 |
| 用例数 | 12 个 | 170+ 个 |
| 运行时间 | 10-15 分钟 | 5-10 分钟 |
| 串联 API 数 | ≥ 5 个 | 1-2 个 |
| 数据一致性 | 直查 DB/ES/MinIO 断言 | 仅验证 HTTP 响应 |
| WS 推送 | 验证通知送达 | 不验证 |
| 触发 | PR to 3.0-dev + nightly | 每 PR |
| Fixture | 多用户 + 多设备 + 群组 | 单用户为主 |

---

## 1. 选取原则

E2E 场景选取遵循 **"三全"原则**：

1. **全链路** - 覆盖从客户端到存储的完整路径（gateway -> service -> MQ -> DB/ES/MinIO）
2. **全一致性** - 验证 HTTP 响应 + DB 落库 + ES 索引 + MinIO 对象 + WS 推送多方一致
3. **全真实** - 用真实全栈（docker-compose），不 mock 任何服务

**E2E 必须覆盖的 6 类用户旅程：**

```
1. 认证与会话    注册->登录->发消息->登出->重登->历史同步
2. 社交关系      加好友->聊天->拉黑->删好友->好友列表一致性
3. 群组协作      建群->加成员->@mention->reaction->撤回->解散
4. 消息可靠性    离线同步 / MQ 故障恢复 / 多设备同步
5. 媒体文件      三步上传->下载->去重->大文件分片
6. 搜索与状态    ES 检索 / 未读数 / 在线状态 / token 刷新
```

**E2E 明确不测的内容：**
- ❌ 单 API 错误路径（L2 职责）
- ❌ 边界条件（空列表/分页边界，L2 职责）
- ❌ 性能基准（L4 职责）
- ❌ 构建可用性（BVT 职责）

---

## 2. E2E 场景清单（12 个）

### 2.1 现有场景（3 个，已实现）

| ID | 名称 | 链路 | 验证点 |
|---|---|---|---|
| SC-01 | `TestScenario_RegisterToFirstMessage` | 注册->搜索->加好友->通过->发消息->sync->history | 认证 + 好友 + 消息基础链路 |
| SC-02 | `TestScenario_GroupChatLifecycle` | 建群->发图->@mention->reaction->撤回->解散 | 群组全生命周期 |
| SC-03 | `TestScenario_FriendFullLifecycle` | 加好友->聊天->删好友->验证好友列表空 | 好友关系全生命周期 |

### 2.2 已规划场景（5 个，test-case-catalog.md 中简述，本文档细化）

| ID | 名称 | 优先级 | 链路 | 验证点 |
|---|---|---|---|---|
| SC-04 | `TestScenario_OfflineMessageSync` | P0 | u2 离线 -> u1 发 3 条 -> u2 上线 sync -> WS 实时推送 | 离线消息不丢、按序、不重复 |
| SC-05 | `TestScenario_MediaUploadFullFlow` | P0 | apply -> PUT -> complete -> download -> dedup -> multipart | 三步上传 + 去重 + 分片 + 下载一致性 |
| SC-06 | `TestScenario_MessageReliability` | P0 | stop rabbitmq -> 发消息失败 -> start -> 重发 -> 验证落库 | MQ 故障不丢消息 + client_msg_id 幂等 |
| SC-07 | `TestScenario_MultiDeviceLogin` | P1 | 设备 A 登录 -> 设备 B 登录 -> A 被踢 -> A token 失效 | 多设备踢人一致性 |
| SC-08 | `TestScenario_LargeGroupFanOut` | P1 | 200+ 成员群 -> 发消息 -> 各成员 sync | 大群读扩散正确性 |

### 2.3 新增场景（4 个，本文档新增）

| ID | 名称 | 优先级 | 链路 | 验证点 |
|---|---|---|---|---|
| SC-09 | `TestScenario_UnreadCountConsistency` | P0 | 发消息 -> 接收方 unread+1 -> UpdateReadAck -> unread=0 -> 跨设备 sync | 未读数跨服务跨设备一致 |
| SC-10 | `TestScenario_MessageRecallVisibility` | P1 | 发消息 -> sync 看到 -> 撤回 -> 另一设备 sync 看到 recalled=true | 撤回可见性跨设备一致 |
| SC-11 | `TestScenario_TokenRefreshFlow` | P1 | 登录 -> 篡改 access_token -> 调 API 失败 -> RefreshToken -> 新 token 调 API 成功 | token 刷新链路 |
| SC-12 | `TestScenario_MessageSearchES` | P1 | 发含关键词消息 -> SearchMessages -> 验证命中 -> 直查 ES 索引存在 | ES 检索与 DB 落库一致 |

### 2.4 统计

- 总场景：12 个（现有 3 + 已规划 5 + 新增 4）
- 预计总耗时：~12 分钟（含全栈启动）
- 覆盖服务：9 个（gateway + 8 业务服务）+ 5 个基础设施（MySQL/Redis/ES/RabbitMQ/MinIO）
- 数据一致性断言点：~30 个（DB/ES/MinIO 直查）

---

## 3. E2E 基础设施设计

当前 `tests/pkg/` 仅有 `client/http.go` + `fixture/`，E2E 测试需要 3 类新基础设施。

### 3.1 WebSocket 客户端（`tests/pkg/client/ws.go`）

**用途**：验证实时推送（SC-04/SC-07/SC-10 依赖）。

```go
package client

import (
    "context"
    "sync"
    "time"

    "github.com/gorilla/websocket"
    "google.golang.org/protobuf/proto"
)

type WSClient struct {
    conn      *websocket.Conn
    accessToken string
    userID    string
    deviceID  string

    mu        sync.Mutex
    notifies  []proto.Message  // 收到的通知缓存
    notifyCh  chan proto.Message
    closed    bool
}

func NewWSClient(cfg *Config, accessToken, userID, deviceID string) (*WSClient, error) {
    url := "ws://" + cfg.Target.WebsocketAddr + "/ws"
    conn, _, err := websocket.DefaultDialer.Dial(url, nil)
    if err != nil {
        return nil, err
    }
    ws := &WSClient{
        conn:        conn,
        accessToken: accessToken,
        userID:      userID,
        deviceID:    deviceID,
        notifyCh:    make(chan proto.Message, 100),
    }
    go ws.readLoop()
    return ws, nil
}

// WaitForNotify 等待指定类型的通知，超时返回 error
func (w *WSClient) WaitForNotify(ctx context.Context, msgType string) (proto.Message, error) {
    for {
        select {
        case <-ctx.Done():
            return nil, ctx.Err()
        case m := <-w.notifyCh:
            // 按 msgType 匹配（实现略）
            return m, nil
        }
    }
}

func (w *WSClient) Close() { w.closed = true; w.conn.Close() }
```

**实现要点**：
- 连接后发送 auth 帧（access_token + device_id）
- readLoop 解析 protobuf 通知帧，按类型分发到 notifyCh
- `WaitForNotify(ctx, type)` 阻塞等待指定类型通知，超时 fail
- 测试结束 `Close()` 释放连接

### 3.2 数据一致性验证包（`tests/pkg/verify/`）

**用途**：直查 DB/ES/MinIO，验证 HTTP 响应与底层存储一致。

#### 3.2.1 `tests/pkg/verify/db.go` - MySQL 直查

```go
package verify

import (
    "database/sql"
    "fmt"

    _ "github.com/go-sql-driver/mysql"
)

type DBVerifier struct {
    db *sql.DB
}

func NewDBVerifier(dsn string) *DBVerifier {
    db, _ := sql.Open("mysql", dsn)
    return &DBVerifier{db: db}
}

// MessageExists 验证 message 表存在指定 message_id 的记录
func (v *DBVerifier) MessageExists(t testing.TB, messageID string) {
    var cnt int
    err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE message_id = ?", messageID).Scan(&cnt)
    require.NoError(t, err)
    require.Equal(t, 1, cnt, "message %s 未落库", messageID)
}

// MessageCount 验证某会话 message 表记录数
func (v *DBVerifier) MessageCount(t testing.TB, convID string, expected int) {
    var cnt int
    err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE conversation_id = ?", convID).Scan(&cnt)
    require.NoError(t, err)
    require.Equal(t, expected, cnt, "会话 %s message 数应为 %d，实际 %d", convID, expected, cnt)
}

// UserTimelineExists 验证 user_timeline 表写扩散记录
func (v *DBVerifier) UserTimelineExists(t testing.TB, userID, convID string, expected int) {
    var cnt int
    err := v.db.QueryRow(
        "SELECT COUNT(*) FROM user_timeline WHERE user_id = ? AND conversation_id = ?",
        userID, convID,
    ).Scan(&cnt)
    require.NoError(t, err)
    require.Equal(t, expected, cnt, "user_timeline 写扩散记录数不符")
}

// FriendRelationExists 验证 friend 关系双向存在
func (v *DBVerifier) FriendRelationExists(t testing.TB, uidA, uidB string) {
    var cnt int
    err := v.db.QueryRow(
        "SELECT COUNT(*) FROM friend WHERE user_id = ? AND friend_id = ?",
        uidA, uidB,
    ).Scan(&cnt)
    require.NoError(t, err)
    require.Equal(t, 1, cnt, "好友关系 %s -> %s 不存在", uidA, uidB)
}

// UnreadCount 验证会话未读数
func (v *DBVerifier) UnreadCount(t testing.TB, userID, convID string, expected int) {
    var cnt int
    err := v.db.QueryRow(
        "SELECT unread_count FROM conversation_member WHERE user_id = ? AND conversation_id = ?",
        userID, convID,
    ).Scan(&cnt)
    require.NoError(t, err)
    require.Equal(t, expected, cnt, "未读数不符")
}
```

#### 3.2.2 `tests/pkg/verify/es.go` - ES 直查

```go
package verify

import (
    "context"
    "testing"

    "github.com/elastic/go-elasticsearch/v8"
    "github.com/stretchr/testify/require"
)

type ESVerifier struct {
    client *elasticsearch.Client
    index  string
}

func NewESVerifier(addr, index string) *ESVerifier {
    cli, _ := elasticsearch.NewClient(elasticsearch.Config{Addresses: []string{addr}})
    return &ESVerifier{client: cli, index: index}
}

// MessageIndexed 验证消息已索引到 ES
func (v *ESVerifier) MessageIndexed(t testing.TB, messageID, keyword string) {
    res, err := v.client.Search(
        v.index,
        v.client.Search.WithBody(strings.NewReader(fmt.Sprintf(
            `{"query":{"bool":{"must":[{"term":{"message_id":"%s"}},{"match":{"content":"%s"}}]}}}`,
            messageID, keyword,
        ))),
        v.client.Search.WithContext(context.Background()),
    )
    require.NoError(t, err)
    var r map[string]interface{}
    json.NewDecoder(res.Body).Decode(&r)
    hits := r["hits"].(map[string]interface{})["total"].(map[string]interface{})["value"].(float64)
    require.Equal(t, float64(1), hits, "ES 未索引消息 %s", messageID)
}
```

#### 3.2.3 `tests/pkg/verify/minio.go` - MinIO 对象验证

```go
package verify

import (
    "context"
    "io"
    "testing"

    "github.com/minio/minio-go/v7"
    "github.com/minio/minio-go/v7/pkg/credentials"
    "github.com/stretchr/testify/require"
)

type MinIOVerifier struct {
    client *minio.Client
}

func NewMinIOVerifier(endpoint, accessKey, secretKey string) *MinIOVerifier {
    cli, _ := minio.New(endpoint, &minio.Options{
        Creds: credentials.NewStaticV4(accessKey, secretKey, ""),
    })
    return &MinIOVerifier{client: cli}
}

// ObjectExists 验证对象存在且返回内容
func (v *MinIOVerifier) ObjectExists(t testing.TB, bucket, key string, expectedContent []byte) {
    obj, err := v.client.GetObject(context.Background(), bucket, key, minio.GetObjectOptions{})
    require.NoError(t, err)
    body, err := io.ReadAll(obj)
    require.NoError(t, err)
    require.Equal(t, expectedContent, body, "MinIO 对象 %s/%s 内容不符", bucket, key)
}

// ObjectRefCount 验证 media_blob_ref ref_count
// 注：ref_count 存在 DB，此方法直查 DB（复用 DBVerifier）
```

### 3.3 Docker Compose 控制包（`tests/pkg/docker/`）

**用途**：SC-06 需要 stop/start rabbitmq 模拟故障。

```go
// tests/pkg/docker/compose.go
package docker

import (
    "os/exec"
    "testing"
    "time"
)

type ComposeController struct {
    workdir string  // docker-compose.yml 所在目录
}

func NewComposeController(workdir string) *ComposeController {
    return &ComposeController{workdir: workdir}
}

// StopService 停止指定服务
func (c *ComposeController) StopService(t testing.TB, service string) {
    cmd := exec.Command("docker", "compose", "stop", service)
    cmd.Dir = c.workdir
    require.NoError(t, cmd.Run(), "停止 %s 失败", service)
}

// StartService 启动指定服务
func (c *ComposeController) StartService(t testing.TB, service string) {
    cmd := exec.Command("docker", "compose", "start", service)
    cmd.Dir = c.workdir
    require.NoError(t, cmd.Run(), "启动 %s 失败", service)
}

// WaitForService 等待服务端口就绪
func (c *ComposeController) WaitForService(t testing.TB, port int, timeout time.Duration) {
    deadline := time.Now().Add(timeout)
    for time.Now().Before(deadline) {
        cmd := exec.Command("nc", "-z", "127.0.0.1", string(port))
        if cmd.Run() == nil {
            return
        }
        time.Sleep(time.Second)
    }
    t.Fatalf("服务端口 %d 未就绪", port)
}
```

### 3.4 基础设施依赖清单

| 组件 | 依赖 | 用途 | Phase |
|---|---|---|---|
| `tests/pkg/client/ws.go` | `github.com/gorilla/websocket` | WS 推送验证 | Phase 1 |
| `tests/pkg/verify/db.go` | `github.com/go-sql-driver/mysql` | DB 直查 | Phase 1 |
| `tests/pkg/verify/es.go` | `github.com/elastic/go-elasticsearch/v8` | ES 直查 | Phase 1 |
| `tests/pkg/verify/minio.go` | `github.com/minio/minio-go/v7` | MinIO 直查 | Phase 1 |
| `tests/pkg/docker/compose.go` | 无（exec docker） | 故障注入 | Phase 1 |

新增 Go 依赖需 `go get` 后 `go mod tidy`。

---

## 4. E2E 场景详细设计

### 4.1 SC-04: 离线消息同步

```go
//go:build func

func TestScenario_OfflineMessageSync(t *testing.T) {
    alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
    bob, _, _ := fixture.RegisterAndLogin(t, HTTP)
    convID := fixture.MakeFriends(t, alice, bob) // 单聊会话

    // Step 1: bob 登出（模拟离线）
    logoutReq := &identity.LogoutReq{RequestId: client.NewRequestID()}
    require.NoError(t, bob.DoAuth("/service/identity/logout", logoutReq, &identity.LogoutRsp{}))

    // Step 2: alice 发 3 条消息
    var lastSeq uint64
    texts := []string{"offline-1", "offline-2", "offline-3"}
    for _, txt := range texts {
        req := &transmite.SendMessageReq{
            RequestId: client.NewRequestID(), ConversationId: convID,
            Content: &msg.MessageContent{
                Type: msg.MessageType_TEXT,
                Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: txt}},
            },
            ClientMsgId: client.NewRequestID(),
        }
        rsp := &transmite.SendMessageRsp{}
        require.NoError(t, alice.DoAuth("/service/transmite/send", req, rsp))
        require.True(t, rsp.Header.Success)
        lastSeq = rsp.Message.SeqId
    }

    // Step 3: bob 重新登录
    bobRelogin, _, _ := fixture.LoginExisting(t, HTTP, bob.UserID)

    // Step 4: bob sync，验证 3 条按序到达
    syncReq := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: 0, Limit: 20}
    syncRsp := &msg.SyncMessagesRsp{}
    require.NoError(t, bobRelogin.DoAuth("/service/message/sync", syncReq, syncRsp))
    require.Len(t, syncRsp.Messages, 3)
    for i, m := range syncRsp.Messages {
        assert.Equal(t, texts[i], m.GetText().Text)
        assert.Less(t, syncRsp.Messages[i-1].SeqId, m.SeqId) // seq 递增
    }

    // Step 5: bob 开 WS，不应收到旧消息推送
    wsBob, err := client.NewWSClient(Cfg, bobRelogin.AccessToken, bobRelogin.UserID, "device-bob")
    require.NoError(t, err)
    defer wsBob.Close()
    ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
    defer cancel()
    _, err = wsBob.WaitForNotify(ctx, "CHAT_MESSAGE_NOTIFY")
    assert.Error(t, err, "不应收到已 sync 的旧消息推送")

    // Step 6: alice 再发 1 条，bob WS 应收到
    newReq := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "realtime-msg"}},
        },
        ClientMsgId: client.NewRequestID(),
    }
    require.NoError(t, alice.DoAuth("/service/transmite/send", newReq, &transmite.SendMessageRsp{}))
    ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
    defer cancel2()
    notify, err := wsBob.WaitForNotify(ctx2, "CHAT_MESSAGE_NOTIFY")
    require.NoError(t, err, "应收到实时消息推送")
    assert.Equal(t, "realtime-msg", notify.GetText().Text)

    // Step 7: bob 增量 sync，仅返回新 1 条
    syncReq2 := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: lastSeq, Limit: 20}
    syncRsp2 := &msg.SyncMessagesRsp{}
    require.NoError(t, bobRelogin.DoAuth("/service/message/sync", syncReq2, syncRsp2))
    require.Len(t, syncRsp2.Messages, 1)

    // Step 8: 数据一致性 - 直查 DB
    DBVerifier.MessageCount(t, convID, 4)
}
```

**验证点**：离线不丢、按序、不重复推送、增量 sync 正确、DB 落库。
**失败含义**：离线消息同步链路 broken。

### 4.2 SC-05: 媒体三步上传全链路

```go
func TestScenario_MediaUploadFullFlow(t *testing.T) {
    user, _, _ := fixture.RegisterAndLogin(t, HTTP)
    content := []byte("e2e-media-content-" + client.NewRequestID()[:8])
    hash := sha256.Sum256(content)
    hashStr := fmt.Sprintf("sha256:%x", hash)

    // Step 1: ApplyUpload
    applyReq := &media.ApplyUploadReq{
        RequestId: client.NewRequestID(), FileName: "e2e.txt",
        FileSize: int64(len(content)), MimeType: "text/plain",
        ContentHash: hashStr, Purpose: media.MediaPurpose_CHAT,
    }
    applyRsp := &media.ApplyUploadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
    require.True(t, applyRsp.Header.Success)
    fileID := applyRsp.FileId
    uploadURL := applyRsp.UploadUrl

    // Step 2: PUT 到 MinIO presigned URL
    req, _ := http.NewRequest("PUT", uploadURL, bytes.NewReader(content))
    putResp, err := http.DefaultClient.Do(req)
    require.NoError(t, err)
    require.Equal(t, 200, putResp.StatusCode)

    // Step 3: CompleteUpload
    completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: fileID}
    completeRsp := &media.CompleteUploadRsp{}
    require.NoError(t, user.DoAuth("/service/media/complete_upload", completeReq, completeRsp))
    require.True(t, completeRsp.Header.Success)

    // Step 4: ApplyDownload + 下载验证内容
    dlReq := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: fileID}
    dlRsp := &media.ApplyDownloadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_download", dlReq, dlRsp))
    dlResp, err := http.Get(dlRsp.DownloadUrl)
    require.NoError(t, err)
    body, _ := io.ReadAll(dlResp.Body)
    assert.Equal(t, content, body, "下载内容与上传不一致")

    // Step 5: 重复 ApplyUpload（相同 hash）-> dedup 返回相同 file_id
    applyReq2 := &media.ApplyUploadReq{
        RequestId: client.NewRequestID(), FileName: "e2e-dup.txt",
        FileSize: int64(len(content)), MimeType: "text/plain",
        ContentHash: hashStr, Purpose: media.MediaPurpose_CHAT,
    }
    applyRsp2 := &media.ApplyUploadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_upload", applyReq2, applyRsp2))
    assert.Equal(t, fileID, applyRsp2.FileId, "dedup 应返回相同 file_id")

    // Step 6: 大文件 multipart（>5MB）
    bigContent := make([]byte, 6*1024*1024) // 6MB
    rand.Read(bigContent)
    bigHash := sha256.Sum256(bigContent)
    initReq := &media.InitMultipartReq{
        RequestId: client.NewRequestID(), FileName: "big.bin",
        FileSize: int64(len(bigContent)), MimeType: "application/octet-stream",
        ContentHash: fmt.Sprintf("sha256:%x", bigHash), PartSize: 2 * 1024 * 1024,
    }
    initRsp := &media.InitMultipartReq{}
    require.NoError(t, user.DoAuth("/service/media/init_multipart", initReq, initRsp))
    uploadID := initRsp.UploadId

    // 分 3 片上传
    parts := make([]*media.CompleteMultipartReq_Part, 0, 3)
    for i := 0; i < 3; i++ {
        partContent := bigContent[i*2*1024*1024 : (i+1)*2*1024*1024]
        applyPartReq := &media.ApplyPartUploadReq{
            RequestId: client.NewRequestID(), UploadId: uploadID, PartNumber: int32(i + 1),
        }
        applyPartRsp := &media.ApplyPartUploadRsp{}
        require.NoError(t, user.DoAuth("/service/media/apply_part_upload", applyPartReq, applyPartRsp))
        partReq, _ := http.NewRequest("PUT", applyPartRsp.UploadUrl, bytes.NewReader(partContent))
        partResp, err := http.DefaultClient.Do(partReq)
        require.NoError(t, err)
        require.Equal(t, 200, partResp.StatusCode)
        parts = append(parts, &media.CompleteMultipartReq_Part{PartNumber: int32(i + 1), ETag: partResp.Header.Get("ETag")})
    }

    completeMultipartReq := &media.CompleteMultipartReq{
        RequestId: client.NewRequestID(), UploadId: uploadID, Parts: parts,
    }
    require.NoError(t, user.DoAuth("/service/media/complete_multipart", completeMultipartReq, &media.CompleteMultipartRsp{}))

    // 下载大文件验证
    bigFileID := initRsp.FileId
    dlReq2 := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: bigFileID}
    dlRsp2 := &media.ApplyDownloadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_download", dlReq2, dlRsp2))
    bigResp, err := http.Get(dlRsp2.DownloadUrl)
    require.NoError(t, err)
    bigBody, _ := io.ReadAll(bigResp.Body)
    assert.Equal(t, bigContent, bigBody, "大文件下载内容不一致")

    // Step 7: 数据一致性 - MinIO 对象存在 + DB ref_count
    MinIOVerifier.ObjectExists(t, "chatnow-media", fileID, content)
    DBVerifier.MediaRefCount(t, fileID, 2) // 原始 + dup = 2
}
```

**验证点**：三步上传 + dedup + multipart + 下载一致性 + MinIO 落对象 + DB ref_count。
**失败含义**：媒体链路 broken。

### 4.3 SC-06: 消息可靠性

```go
func TestScenario_MessageReliability(t *testing.T) {
    alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
    bob, _, _ := fixture.RegisterAndLogin(t, HTTP)
    convID := fixture.MakeFriends(t, alice, bob)

    compose := docker.NewComposeController("..") // docker-compose.yml 在仓库根

    // Step 1: 停止 rabbitmq
    compose.StopService(t, "rabbitmq")

    // Step 2: alice 发消息，应失败（MQ 投递失败）
    clientMsgID := client.NewRequestID()
    sendReq := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "mq-fail-msg"}},
        },
        ClientMsgId: clientMsgID,
    }
    sendRsp := &transmite.SendMessageRsp{}
    err := alice.DoAuth("/service/transmite/send", sendReq, sendRsp)
    // 预期：响应 success=false 或 HTTP 超时
    require.True(t, err != nil || !sendRsp.Header.Success, "MQ 故障时发消息应失败")

    // Step 3: 启动 rabbitmq + 等待就绪
    compose.StartService(t, "rabbitmq")
    compose.WaitForService(t, 5672, 30*time.Second)
    time.Sleep(5 * time.Second) // 等 transmite 服务重连 MQ

    // Step 4: 用相同 client_msg_id 重发，应成功（幂等）
    sendReq2 := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "mq-fail-msg"}},
        },
        ClientMsgId: clientMsgID, // 相同 client_msg_id
    }
    sendRsp2 := &transmite.SendMessageRsp{}
    require.NoError(t, alice.DoAuth("/service/transmite/send", sendReq2, sendRsp2))
    require.True(t, sendRsp2.Header.Success, "重发应成功")
    msgID := sendRsp2.Message.MessageId

    // Step 5: bob sync 验证收到
    syncReq := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: 0, Limit: 10}
    syncRsp := &msg.SyncMessagesRsp{}
    require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
    require.Len(t, syncRsp.Messages, 1)
    assert.Equal(t, msgID, syncRsp.Messages[0].MessageId)

    // Step 6: 数据一致性 - DB 仅 1 条（不重复）
    DBVerifier.MessageCount(t, convID, 1)
}
```

**验证点**：MQ 故障发消息失败 + 恢复后重发成功 + client_msg_id 幂等去重 + DB 不重复。
**失败含义**：消息可靠性链路 broken。

### 4.4 SC-07: 多设备登录

```go
func TestScenario_MultiDeviceLogin(t *testing.T) {
    // 设备 A 登录
    deviceA, userID, _ := fixture.RegisterAndLogin(t, HTTP)
    deviceA.DeviceID = "device-A"

    // 验证 A 能调 API
    profileReq := &identity.GetProfileReq{RequestId: client.NewRequestID()}
    require.NoError(t, deviceA.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}))

    // 设备 B 登录同用户
    deviceB := client.NewHTTPClient(Cfg)
    deviceB.DeviceID = "device-B"
    loginReq := &identity.LoginReq{
        RequestId: client.NewRequestID(), Username: "e2e_user_" + userID[:8],
        Password: "E2e@123456", DeviceId: "device-B",
    }
    loginRsp := &identity.LoginRsp{}
    require.NoError(t, deviceB.DoNoAuth("/service/identity/login", loginReq, loginRsp))
    deviceB.AccessToken = loginRsp.AccessToken

    // 设备 A 的 token 应失效（被踢）
    err := deviceA.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{})
    assert.Error(t, err, "设备 A 被踢后 token 应失效")

    // 设备 B 仍可调 API
    require.NoError(t, deviceB.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}))

    // 数据一致性 - DB/Redis 中 A 的 session 已删除
    DBVerifier.UserSessionCount(t, userID, 1) // 仅 B 的 session
}
```

**验证点**：多设备踢人 + 旧 token 失效 + 新 token 可用 + session 一致。
**失败含义**：多设备登录链路 broken。

### 4.5 SC-08: 大群读扩散

```go
func TestScenario_LargeGroupFanOut(t *testing.T) {
    owner, _, _ := fixture.RegisterAndLogin(t, HTTP)

    // 批量注册 200 成员
    members := make([]*client.HTTPClient, 0, 200)
    memberIDs := make([]string, 0, 200)
    for i := 0; i < 200; i++ {
        m, uid, _ := fixture.RegisterAndLogin(t, HTTP)
        members = append(members, m)
        memberIDs = append(memberIDs, uid)
    }

    // 建群
    name := "e2e-large-group"
    createReq := &conversation.CreateConversationReq{
        RequestId: client.NewRequestID(), Type: conversation.ConversationType_GROUP,
        Name: &name, MemberIds: memberIDs,
    }
    createRsp := &conversation.CreateConversationRsp{}
    require.NoError(t, owner.DoAuth("/service/conversation/create", createReq, createRsp))
    convID := createRsp.Conversation.ConversationId

    // owner 发消息
    sendReq := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "large-group-msg"}},
        },
        ClientMsgId: client.NewRequestID(),
    }
    sendRsp := &transmite.SendMessageRsp{}
    require.NoError(t, owner.DoAuth("/service/transmite/send", sendReq, sendRsp))
    require.True(t, sendRsp.Header.Success)
    msgID := sendRsp.Message.MessageId

    // 抽样 10 个成员验证 sync 收到
    for i := 0; i < 10; i++ {
        idx := i * 20 // 每隔 20 个抽一个
        syncReq := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: 0, Limit: 10}
        syncRsp := &msg.SyncMessagesRsp{}
        require.NoError(t, members[idx].DoAuth("/service/message/sync", syncReq, syncRsp))
        require.Len(t, syncRsp.Messages, 1, "成员 %d 未收到消息", idx)
        assert.Equal(t, msgID, syncRsp.Messages[0].MessageId)
    }

    // 数据一致性 - 读扩散：message 表仅 1 条，user_timeline 201 条（200 成员 + owner）
    DBVerifier.MessageCount(t, convID, 1)
    DBVerifier.UserTimelineCount(t, convID, 201)
}
```

**验证点**：大群读扩散 + 消息不丢 + 读写扩散 DB 一致。
**失败含义**：大群消息分发 broken。

### 4.6 SC-09: 未读数一致性（新增）

```go
func TestScenario_UnreadCountConsistency(t *testing.T) {
    alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
    bob, _, _ := fixture.RegisterAndLogin(t, HTTP)
    convID := fixture.MakeFriends(t, alice, bob)

    // Step 1: alice 发 3 条消息
    for i := 0; i < 3; i++ {
        req := &transmite.SendMessageReq{
            RequestId: client.NewRequestID(), ConversationId: convID,
            Content: &msg.MessageContent{
                Type: msg.MessageType_TEXT,
                Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("unread-%d", i)}},
            },
            ClientMsgId: client.NewRequestID(),
        }
        require.NoError(t, alice.DoAuth("/service/transmite/send", req, &transmite.SendMessageRsp{}))
    }

    // Step 2: bob ListConversations，验证 unread_count=3
    listReq := &conversation.ListConversationsReq{RequestId: client.NewRequestID()}
    listRsp := &conversation.ListConversationsRsp{}
    require.NoError(t, bob.DoAuth("/service/conversation/list", listReq, listRsp))
    var bobConv *conversation.Conversation
    for _, c := range listRsp.Conversations {
        if c.ConversationId == convID {
            bobConv = c
            break
        }
    }
    require.NotNil(t, bobConv)
    assert.Equal(t, uint64(3), bobConv.UnreadCount, "bob 未读数应为 3")

    // Step 3: 数据一致性 - DB conversation_member.unread_count=3
    DBVerifier.UnreadCount(t, bob.UserID, convID, 3)

    // Step 4: bob UpdateReadAck（读到最后一条 seq）
    ackReq := &msg.UpdateReadAckReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        ReadSeq: bobConv.LastSeq,
    }
    require.NoError(t, bob.DoAuth("/service/message/update_read_ack", ackReq, &msg.UpdateReadAckRsp{}))

    // Step 5: bob 再次 ListConversations，unread_count=0
    listRsp2 := &conversation.ListConversationsRsp{}
    require.NoError(t, bob.DoAuth("/service/conversation/list", listReq, listRsp2))
    for _, c := range listRsp2.Conversations {
        if c.ConversationId == convID {
            assert.Equal(t, uint64(0), c.UnreadCount, "read ack 后未读数应清零")
        }
    }

    // Step 6: bob 设备 B 登录，unread_count 同步为 0
    bobDevB := client.NewHTTPClient(Cfg)
    // ... 登录设备 B（略）
    listRsp3 := &conversation.ListConversationsRsp{}
    require.NoError(t, bobDevB.DoAuth("/service/conversation/list", listReq, listRsp3))
    for _, c := range listRsp3.Conversations {
        if c.ConversationId == convID {
            assert.Equal(t, uint64(0), c.UnreadCount, "设备 B 未读数应同步为 0")
        }
    }

    // Step 7: 数据一致性 - DB unread_count=0
    DBVerifier.UnreadCount(t, bob.UserID, convID, 0)
}
```

**验证点**：发消息 unread+1 + read ack 清零 + 跨设备同步 + DB 一致。
**失败含义**：未读数链路 broken。

### 4.7 SC-10: 撤回消息可见性（新增）

```go
func TestScenario_MessageRecallVisibility(t *testing.T) {
    alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
    bob, _, _ := fixture.RegisterAndLogin(t, HTTP)
    convID := fixture.MakeFriends(t, alice, bob)

    // alice 发消息
    sendReq := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "will-recall"}},
        },
        ClientMsgId: client.NewRequestID(),
    }
    sendRsp := &transmite.SendMessageRsp{}
    require.NoError(t, alice.DoAuth("/service/transmite/send", sendReq, sendRsp))
    msgID := sendRsp.Message.MessageId

    // bob 设备 A sync，看到消息内容
    syncReq := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: 0, Limit: 10}
    syncRsp := &msg.SyncMessagesRsp{}
    require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
    require.Len(t, syncRsp.Messages, 1)
    assert.Equal(t, "will-recall", syncRsp.Messages[0].GetText().Text)
    assert.False(t, syncRsp.Messages[0].Recalled)

    // alice 撤回
    recallReq := &msg.RecallMessageReq{RequestId: client.NewRequestID(), ConversationId: convID, MessageId: msgID}
    require.NoError(t, alice.DoAuth("/service/message/recall", recallReq, &msg.RecallMessageRsp{}))

    // bob 设备 B 登录，sync 看到 recalled=true，内容清空
    bobDevB := client.NewHTTPClient(Cfg)
    // ... 登录设备 B（略）
    syncRsp2 := &msg.SyncMessagesRsp{}
    require.NoError(t, bobDevB.DoAuth("/service/message/sync", syncReq, syncRsp2))
    require.Len(t, syncRsp2.Messages, 1)
    assert.True(t, syncRsp2.Messages[0].Recalled, "撤回后应标记 recalled=true")
    assert.Empty(t, syncRsp2.Messages[0].GetText().Text, "撤回后内容应清空")

    // 数据一致性 - DB message.recalled=true
    DBVerifier.MessageRecalled(t, msgID, true)
}
```

**验证点**：撤回标记跨设备一致 + 内容清空 + DB recalled 字段。
**失败含义**：撤回链路 broken。

### 4.8 SC-11: Token 刷新流程（新增）

```go
func TestScenario_TokenRefreshFlow(t *testing.T) {
    user, _, _ := fixture.RegisterAndLogin(t, HTTP)
    validToken := user.AccessToken
    refreshToken := user.RefreshToken

    // Step 1: 篡改 access_token，调 API 失败
    user.AccessToken = "tampered.invalid.token"
    profileReq := &identity.GetProfileReq{RequestId: client.NewRequestID()}
    err := user.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{})
    assert.Error(t, err, "篡改 token 后应鉴权失败")

    // Step 2: 用 refresh_token 刷新
    user.AccessToken = validToken // 先恢复（刷新接口可能需要旧 token）
    refreshReq := &identity.RefreshTokenReq{
        RequestId: client.NewRequestID(), UserId: user.UserID, RefreshToken: refreshToken,
    }
    refreshRsp := &identity.RefreshTokenRsp{}
    require.NoError(t, user.DoNoAuth("/service/identity/refresh_token", refreshReq, refreshRsp))
    require.True(t, refreshRsp.Header.Success)
    require.NotEmpty(t, refreshRsp.AccessToken)
    require.NotEqual(t, validToken, refreshRsp.AccessToken, "新 token 应不同于旧 token")

    // Step 3: 新 token 调 API 成功
    user.AccessToken = refreshRsp.AccessToken
    require.NoError(t, user.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}))

    // Step 4: 旧 token 应失效（可选，取决于实现）
    user.AccessToken = validToken
    err = user.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{})
    // 预期：旧 token 失效（若实现单 token）或仍可用（若允许多 token）
    // 此处宽松断言：不强制要求旧 token 失效
    _ = err

    // 数据一致性 - DB/Redis session 更新
    DBVerifier.UserSessionExists(t, user.UserID, refreshRsp.AccessToken)
}
```

**验证点**：token 刷新 + 新 token 可用 + session 更新。
**失败含义**：token 刷新链路 broken。

### 4.9 SC-12: 消息搜索 ES 一致性（新增）

```go
func TestScenario_MessageSearchES(t *testing.T) {
    alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
    bob, _, _ := fixture.RegisterAndLogin(t, HTTP)
    convID := fixture.MakeFriends(t, alice, bob)

    // 发含特殊关键词的消息
    keyword := "e2e-search-keyword-" + client.NewRequestID()[:8]
    sendReq := &transmite.SendMessageReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hello " + keyword + " world"}},
        },
        ClientMsgId: client.NewRequestID(),
    }
    sendRsp := &transmite.SendMessageRsp{}
    require.NoError(t, alice.DoAuth("/service/transmite/send", sendReq, sendRsp))
    msgID := sendRsp.Message.MessageId

    // 等待 ES 索引（异步，需 polling）
    time.Sleep(2 * time.Second)

    // SearchMessages 命中
    searchReq := &msg.SearchMessagesReq{
        RequestId: client.NewRequestID(), ConversationId: convID,
        Keyword: keyword, Page: &common.PageRequest{Limit: 10},
    }
    searchRsp := &msg.SearchMessagesRsp{}
    require.NoError(t, bob.DoAuth("/service/message/search", searchReq, searchRsp))
    require.True(t, searchRsp.Header.Success)
    require.Len(t, searchRsp.Messages, 1, "搜索应命中 1 条")
    assert.Equal(t, msgID, searchRsp.Messages[0].MessageId)

    // 数据一致性 - ES 索引存在
    ESVerifier.MessageIndexed(t, msgID, keyword)

    // 数据一致性 - DB 也有该消息
    DBVerifier.MessageExists(t, msgID)
}
```

**验证点**：ES 索引 + 搜索命中 + DB 一致。
**失败含义**：ES 索引链路 broken。

---

## 5. CI 集成

### 5.1 scenario job 增强

E2E 场景在现有 `scenario` job 中运行，但 SC-06 需要 docker compose 控制权限：

```yaml
scenario:
  needs: func
  runs-on: ubuntu-22.04
  if: github.event_name == 'schedule' || (github.event_name == 'pull_request' && github.base_ref == '3.0-dev')
  steps:
    - uses: actions/checkout@v4
    - uses: actions/setup-go@v5
      with: { go-version: '1.23' }
    - run: sudo apt-get install -y protobuf-compiler netcat-openbsd
    - run: docker compose up -d --build
    - run: ./scripts/wait_for_services.sh
    - run: cd tests && make proto && go mod download
    - run: cd tests && make test-scenario
    - if: always()
      run: docker compose down -v
```

**注意**：SC-06 在 scenario job 中 stop/start rabbitmq，不影响其他 job（每 job 独立 docker compose 实例）。

### 5.2 E2E 运行时间预算

| 场景 | 预计耗时 |
|---|---|
| SC-01 ~ 03（现有） | 30s |
| SC-04 离线同步 | 60s（含 WS 等待） |
| SC-05 媒体全链路 | 90s（含 6MB 上传） |
| SC-06 消息可靠性 | 120s（含 MQ stop/start） |
| SC-07 多设备 | 30s |
| SC-08 大群 | 180s（含 200 注册） |
| SC-09 未读数 | 30s |
| SC-10 撤回可见性 | 30s |
| SC-11 token 刷新 | 20s |
| SC-12 ES 搜索 | 30s（含 ES 索引等待） |
| **总计** | **~10min** |

### 5.3 本地运行

```bash
# 跑全部 E2E 场景
docker compose up -d
cd tests && make test-scenario

# 跑单个场景
cd tests && go test -tags=func ./func/... -run TestScenario_OfflineMessageSync -v
```

---

## 6. E2E 失败处理流程

### 6.1 失败分类

| 失败类型 | 典型表现 | 处理 |
|---|---|---|
| 基础设施未就绪 | WS 连接失败 / DB 连接失败 | 检查 docker-compose、wait_for_services.sh |
| 链路 broken | sync 收不到消息 / unread 不更新 | 按链路定位：gateway -> service -> MQ -> DB |
| 数据不一致 | HTTP 响应正确但 DB/ES 不符 | 检查异步落库逻辑、MQ 消费 |
| WS 推送缺失 | 消息发送成功但接收方未收到通知 | 检查 presence/gateway WS 推送 |
| Flaky | 偶发失败（ES 索引延迟、MQ 重连） | 增加等待/重试，但不超过 3 次 |

### 6.2 Flaky 容忍度

E2E 允许有限 flaky（与 BVT 的零容忍不同）：
- ES 索引延迟：用 polling + 超时替代固定 sleep
- MQ 重连：SC-06 等 5s 后重试
- WS 推送：WaitForNotify 超时 5s
- 同一用例连续 3 次 fail 才标记真实 fail

### 6.3 数据隔离

每个 E2E 场景用独立用户（`fixture.RegisterAndLogin` 每次注册新用户），不共享 fixture，避免相互干扰。SC-08 大群场景注册 200 用户，跑后不清理（依赖 DB 隔离级别 + 测试库独立）。

---

## 7. E2E 维护规则

### 7.1 新增 E2E 场景的条件

1. **跨服务链路** - 至少经过 2 个业务服务
2. **数据一致性** - 验证 HTTP 响应 + 至少 1 个底层存储（DB/ES/MinIO）
3. **不可被 L2 覆盖** - L2 无法验证的链路正确性
4. **真实用户旅程** - 模拟真实操作路径，非拼凑

### 7.2 E2E 上限

- **硬上限 20 个** - 超过则总时长 > 15min，失去"合并前跑"的意义
- 当前 12 个，有 8 个余量

### 7.3 从 E2E 移除用例的条件

1. 对应功能被废弃
2. 用例持续 flaky 且无法修复（降级到 L2）
3. 用例耗时 > 5min（优化或降级）

---

## 8. 统计

| 维度 | 数值 |
|---|---|
| E2E 场景总数 | 12 |
| 现有场景 | 3（SC-01~03） |
| 已规划场景 | 5（SC-04~08，test-case-catalog 中简述） |
| 新增场景 | 4（SC-09~12） |
| 预计总耗时 | ~10min |
| 数据一致性断言点 | ~30 个 |
| 覆盖服务 | 9（gateway + 8 业务） |
| 新增基础设施 | 3（ws.go / verify/ / docker/） |
| Build tag | `//go:build func`（与 L2 共享，用 `TestScenario_` 前缀区分） |
| Makefile 目标 | `make test-scenario`（现有，无需新增） |
| CI job | `scenario`（现有，无需新增） |

---

## 9. Phase 归属

| Phase | 交付物 | 说明 |
|---|---|---|
| Phase 1 | SC-04 / SC-05 / SC-09 / SC-10 | 核心消息 + 媒体 + 未读 + 撤回，需 ws.go + verify/ |
| Phase 1 | `tests/pkg/client/ws.go` | WS 客户端 |
| Phase 1 | `tests/pkg/verify/db.go` + `es.go` + `minio.go` | 数据一致性验证包 |
| Phase 2 | SC-06 / SC-07 / SC-11 | 可靠性 + 多设备 + token 刷新，需 docker/ |
| Phase 2 | `tests/pkg/docker/compose.go` | docker compose 控制 |
| Phase 3 | SC-08 / SC-12 | 大群 + ES 搜索，需批量 fixture + ES 索引验证 |

Phase 1 的 plan（`2026-07-08-phase1-*.md`，待创建）应将 SC-04/05/09/10 + 基础设施纳入 Task 范围。

---

## 10. 与其他测试文档的关系

| 文档 | 定位 | 关系 |
|---|---|---|
| `2026-07-08-go-testing-design.md` | 测试架构总设计 | E2E 是 L3 层，本文档细化 |
| `2026-07-08-test-case-catalog.md` | L2/L3/L4 用例目录 | SC-04~08 在目录中简述，本文档给出完整代码 + 新增 SC-09~12 |
| `2026-07-08-bvt-test-design.md` | BVT 专项设计 | BVT 是 L1，E2E 是 L3，互补 |
| `2026-07-08-phase0-ci-infrastructure.md` | Phase 0 CI 实施 | Phase 0 搭 CI，E2E 在 Phase 1/2/3 落地 |
| 本文档 | E2E 专项设计 | 独立设计，Phase 1/2/3 实现时落地 |
