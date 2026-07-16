# Phase 2 C++ 覆盖缺口补齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 avatar URL、Gateway trace、MQ trace、media object key 与 magic sniff 的等价行为覆盖融入现有 Go 全栈测试框架，使 Task 22 删除的 C++ 测试具备可追踪的 Go 黑盒替代。

**Architecture:** 共享请求、上传和 DB 查询能力分别下沉到 `tests/pkg/client`、`tests/pkg/fixture`、`tests/pkg/verify`；具体行为断言放入现有 `tests/func` 服务文件，并复用统一 `TestMain`、cleanup、配置和 build tag。不恢复 C++ 测试，不新增平行测试框架，不修改生产代码。

**Tech Stack:** Go 1.23、testing、testify、protobuf over HTTP、gorilla/websocket、MySQL 直查、真实全栈。

## Global Constraints

- 纯 Go 测试（testify + 标准 testing），不引入 C++ 测试，不 mock 服务，用真实全栈。
- 黑盒行为测试：通过 HTTP + protobuf 外部 API 验证服务行为，不测 C++ 内部实现。
- 所有共享能力进入 `tests/pkg/{client,fixture,verify}`；行为断言只放在 `tests/func`。
- 继续复用 `tests/func/setup_test.go` 的 `TestMain`、`cleanup.CleanupAll`、`Cfg` 与 `HTTP`。
- `tests/func` 文件保持 `//go:build func`；用例使用 `FN-AM-06`、`FN-WS-08`、`FN-ID-08`、`FN-MD-19`、`FN-MD-20`。
- Fixture 不做断言（除不可继续时 `t.Fatal`），返回关键 ID；verifier 负责稳定查询，测试负责业务断言。
- 不恢复 Task 22 删除的任何 `.cc`，不修改生产代码。
- 用户明确要求本地不执行测试：实现者不得运行 `go test`、`go vet`、编译、Docker 或全栈命令；仅运行 gofmt、diff whitespace 检查和静态代码审查，并明确记录未运行测试。

---

### Task 23: Trace-aware HTTP client + Gateway/MQ trace 用例

**Files:**
- Modify: `tests/pkg/client/http.go`
- Modify: `tests/func/auth_middleware_test.go`
- Modify: `tests/func/ws_notify_test.go`

**Interfaces:**
- Produces: `HTTPClient.DoWithTrace(path, req, resp, accessToken, traceID) (http.Header, error)`
- Consumes: 现有 `fixture.MakeFriends`、`fixture.ConnectWS`、`client.WSClient.WaitForNotify`

- [ ] **Step 1: 抽取 HTTP 发送核心并增加 trace-aware API**

在 `tests/pkg/client/http.go` 中保留现有 public API，新增：

```go
func (c *HTTPClient) DoWithTrace(path string, req proto.Message, resp proto.Message, accessToken, traceID string) (http.Header, error) {
    return c.do(path, req, resp, accessToken, traceID)
}
```

把现有 `Do` 的主体移入私有方法：

```go
func (c *HTTPClient) do(path string, req proto.Message, resp proto.Message, accessToken, traceID string) (http.Header, error) {
    body, err := proto.Marshal(req)
    if err != nil {
        return nil, fmt.Errorf("marshal request: %w", err)
    }
    httpReq, err := http.NewRequest("POST", c.baseURL+path, bytes.NewReader(body))
    if err != nil {
        return nil, fmt.Errorf("create request: %w", err)
    }
    httpReq.Header.Set("Content-Type", "application/x-protobuf")
    if accessToken != "" {
        httpReq.Header.Set("Authorization", "Bearer "+accessToken)
    }
    if traceID != "" {
        httpReq.Header.Set("X-Trace-Id", traceID)
    }
    httpResp, err := c.client.Do(httpReq)
    if err != nil {
        return nil, fmt.Errorf("http request: %w", err)
    }
    defer httpResp.Body.Close()
    respBody, err := io.ReadAll(httpResp.Body)
    if err != nil {
        return httpResp.Header.Clone(), fmt.Errorf("read response: %w", err)
    }
    headers := httpResp.Header.Clone()
    if httpResp.StatusCode != http.StatusOK {
        return headers, fmt.Errorf("http status %d: %s", httpResp.StatusCode, string(respBody))
    }
    if err := proto.Unmarshal(respBody, resp); err != nil {
        return headers, fmt.Errorf("unmarshal response: %w", err)
    }
    return headers, nil
}
```

现有 `Do` 改为：

```go
func (c *HTTPClient) Do(path string, req proto.Message, resp proto.Message, accessToken string) error {
    _, err := c.do(path, req, resp, accessToken, "")
    return err
}
```

- [ ] **Step 2: 添加 FN-AM-06 Gateway trace 响应测试代码**

在 `tests/func/auth_middleware_test.go` 追加：

```go
// FN-AM-06 | P1 | trace | Gateway 回传客户端提供的合法 X-Trace-Id
func TestFN_AM_GatewayTraceHeader(t *testing.T) {
    authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
    traceID := "0123456789abcdef0123456789abcdef"
    req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
    rsp := &identity.GetProfileRsp{}

    headers, err := authed.DoWithTrace(
        "/service/identity/get_profile", req, rsp, authed.AccessToken, traceID,
    )
    require.NoError(t, err)
    require.True(t, rsp.Header.Success)
    assert.Equal(t, traceID, headers.Get("X-Trace-Id"))
}
```

- [ ] **Step 3: 添加 FN-WS-08 MQ trace 透传测试代码**

给 `tests/func/ws_notify_test.go` 增加 message/transmite import，并追加：

```go
// FN-WS-08 | P1 | trace | Gateway trace 经 MQ 透传到接收方 WS notify
func TestFN_WS_MQTracePropagation(t *testing.T) {
    alice, bob, convID := fixture.MakeFriends(t, HTTP)
    wsBob := fixture.ConnectWS(t, bob)
    defer wsBob.Close()
    time.Sleep(500 * time.Millisecond)

    traceID := "fedcba9876543210fedcba9876543210"
    req := &transmite.SendMessageReq{
        RequestId:      client.NewRequestID(),
        ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "trace-propagation"}},
        },
        ClientMsgId: client.NewRequestID(),
    }
    rsp := &transmite.SendMessageRsp{}
    _, err := alice.DoWithTrace("/service/transmite/send", req, rsp, alice.AccessToken, traceID)
    require.NoError(t, err)
    require.True(t, rsp.Header.Success)

    ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
    defer cancel()
    notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
    require.NoError(t, err)
    assert.Equal(t, traceID, notify.GetTraceId())
}
```

- [ ] **Step 4: 静态检查与提交**

仅运行 gofmt 与 `git diff --check`，不得运行测试/编译/vet。提交：

```bash
git add tests/pkg/client/http.go tests/func/auth_middleware_test.go tests/func/ws_notify_test.go
git commit -m "test(trace): add Gateway and MQ trace black-box coverage"
```

---

### Task 24: Media framework + avatar/object-key 用例

**Files:**
- Modify: `tests/pkg/fixture/media.go`
- Modify: `tests/pkg/verify/db.go`
- Modify: `tests/func/identity_test.go`
- Modify: `tests/func/media_test.go`

**Interfaces:**
- Produces: `fixture.UploadFileForPurpose(...) string`
- Produces: `verify.MediaFileRecord`、`DBVerifier.MediaFile(...)`
- Preserves: `fixture.UploadFile(...) string`

- [ ] **Step 1: 把单段上传改为按 purpose 可复用**

把现有 `UploadFile` 改为 wrapper：

```go
func UploadFile(t testing.TB, c *client.HTTPClient, content []byte, mime string) string {
    return UploadFileForPurpose(t, c, content, mime, media.MediaPurpose_CHAT)
}
```

把原函数主体移动到：

```go
func UploadFileForPurpose(t testing.TB, c *client.HTTPClient, content []byte, mime string, purpose media.MediaPurpose) string
```

函数主体保持原流程，仅把 `ApplyUploadReq.Purpose` 从固定 CHAT 改为参数 `purpose`。

- [ ] **Step 2: 增加 DB media record 查询接口**

在 `tests/pkg/verify/db.go` 增加：

```go
type MediaFileRecord struct {
    Bucket    string
    ObjectKey string
    Status    int
}

func (v *DBVerifier) MediaFile(t testing.TB, fileID string) MediaFileRecord {
    t.Helper()
    var record MediaFileRecord
    err := v.db.QueryRow(
        "SELECT bucket, object_key, status FROM media_file WHERE file_id = ?", fileID,
    ).Scan(&record.Bucket, &record.ObjectKey, &record.Status)
    if err != nil {
        t.Fatalf("query media_file %s: %v", fileID, err)
    }
    return record
}
```

- [ ] **Step 3: 添加 FN-ID-08 avatar upload/profile URL 用例**

在 `tests/func/identity_test.go` 添加 `strings` 与 media proto import，追加：

```go
// FN-ID-08 | P1 | happy path | 上传头像后更新 profile 并返回公开 avatar URL
func TestFN_ID_UpdateProfileAvatarUpload(t *testing.T) {
    authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
    content := append([]byte{0xFF, 0xD8, 0xFF, 0xE0}, []byte(client.NewRequestID())...)
    fileID := fixture.UploadFileForPurpose(t, authed, content, "image/jpeg", media.MediaPurpose_AVATAR)

    req := &identity.UpdateProfileReq{
        RequestId:    client.NewRequestID(),
        AvatarFileId: &fileID,
    }
    rsp := &identity.UpdateProfileRsp{}
    require.NoError(t, authed.DoAuth("/service/identity/update_profile", req, rsp))
    require.True(t, rsp.Header.Success)
    require.NotNil(t, rsp.UserInfo)
    require.NotEmpty(t, rsp.UserInfo.AvatarUrl)
    assert.True(t, strings.HasSuffix(rsp.UserInfo.AvatarUrl, "/avatar/"+fileID))

    getRsp := &identity.GetProfileRsp{}
    require.NoError(t, authed.DoAuth(
        "/service/identity/get_profile",
        &identity.GetProfileReq{RequestId: client.NewRequestID()},
        getRsp,
    ))
    require.True(t, getRsp.Header.Success)
    require.NotNil(t, getRsp.UserInfo)
    assert.Equal(t, rsp.UserInfo.AvatarUrl, getRsp.UserInfo.AvatarUrl)
}
```

- [ ] **Step 4: 添加 FN-MD-19 object key 用例**

给 `tests/func/media_test.go` 增加 `path`、`regexp`、`strings` 与 verify import，追加：

```go
// FN-MD-19 | P1 | consistency | CHAT 与 AVATAR 使用各自的 object key 布局
func TestFN_MD_ObjectKeyLayout(t *testing.T) {
    authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
    dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
    defer dbV.Close()

    chatContent := []byte("fn-md-19-chat-" + client.NewRequestID())
    chatHash := sha256.Sum256(chatContent)
    chatHex := fmt.Sprintf("%x", chatHash)
    chatID := fixture.UploadFile(t, authed, chatContent, "text/plain")
    chatRecord := dbV.MediaFile(t, chatID)
    assert.Equal(t, "chatnow-media-private", chatRecord.Bucket)
    assert.Regexp(t, regexp.MustCompile(`^chat/[0-9]{4}/[0-9]{2}/[0-9]{2}/[0-9a-f]{2}/[0-9a-f]{64}$`), chatRecord.ObjectKey)
    assert.Equal(t, chatHex, path.Base(chatRecord.ObjectKey))
    assert.True(t, strings.Contains(chatRecord.ObjectKey, "/"+chatHex[:2]+"/"))

    avatarContent := append([]byte{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, []byte(client.NewRequestID())...)
    avatarHash := sha256.Sum256(avatarContent)
    avatarHex := fmt.Sprintf("%x", avatarHash)
    avatarID := fixture.UploadFileForPurpose(t, authed, avatarContent, "image/png", media.MediaPurpose_AVATAR)
    avatarRecord := dbV.MediaFile(t, avatarID)
    assert.Equal(t, "chatnow-media-public", avatarRecord.Bucket)
    assert.Equal(t, "avatar/"+avatarHex, avatarRecord.ObjectKey)
}
```

- [ ] **Step 5: 静态检查与提交**

仅运行 gofmt 与 `git diff --check`，不得运行测试/编译/vet。提交：

```bash
git add tests/pkg/fixture/media.go tests/pkg/verify/db.go tests/func/identity_test.go tests/func/media_test.go
git commit -m "test(media): add avatar and object-key black-box coverage"
```

---

### Task 25: Magic sniff quarantine 最终一致性用例

**Files:**
- Modify: `tests/func/media_test.go`

**Interfaces:**
- Consumes: `fixture.UploadFile`（Task 24 保持兼容）
- Consumes: `DBVerifier.MediaFile`（Task 24）

- [ ] **Step 1: 添加 FN-MD-20 magic mismatch quarantine 用例**

在 `tests/func/media_test.go` 增加 `time` import，追加：

```go
// FN-MD-20 | P1 | security | 声明 JPEG、实际 PE magic 的文件最终被隔离且不可下载
func TestFN_MD_MagicMismatchQuarantined(t *testing.T) {
    authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
    content := append([]byte{'M', 'Z', 0, 0, 0, 0, 0, 0}, []byte(client.NewRequestID())...)
    fileID := fixture.UploadFile(t, authed, content, "image/jpeg")

    dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
    defer dbV.Close()
    require.Eventually(t, func() bool {
        return dbV.MediaFile(t, fileID).Status == 3
    }, 90*time.Second, 2*time.Second, "magic mismatch 文件应进入 QUARANTINED")

    req := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: fileID}
    rsp := &media.ApplyDownloadRsp{}
    require.NoError(t, authed.DoAuth("/service/media/apply_download", req, rsp))
    assert.False(t, rsp.Header.Success)
    assert.Equal(t, int32(5008), rsp.Header.ErrorCode)
}
```

- [ ] **Step 2: 静态检查与提交**

确认轮询闭包只比较状态，不在瞬时未就绪时调用 `require`/`assert`；仅运行 gofmt 与 `git diff --check`。提交：

```bash
git add tests/func/media_test.go
git commit -m "test(media): cover asynchronous magic-sniff quarantine"
```

---

## 验收标准

1. `tests/pkg/client` 提供 trace-aware HTTP 请求并保留原有 API。
2. `tests/pkg/fixture` 提供按 media purpose 上传，原 `UploadFile` 调用方不变。
3. `tests/pkg/verify` 提供稳定的 `MediaFile` 只读查询。
4. `FN-AM-06`、`FN-WS-08`、`FN-ID-08`、`FN-MD-19`、`FN-MD-20` 均存在并遵循框架命名/分层。
5. 不恢复 C++ 测试，不修改生产代码，不建立新测试体系。
6. 每个任务通过独立静态 reviewer；所有 Critical/Important 清零。
7. 按用户指令不运行测试，最终状态明确标注运行验证缺失。
