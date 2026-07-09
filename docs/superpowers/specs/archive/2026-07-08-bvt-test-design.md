# ChatNow BVT 测试设计

> **状态**: 设计完成，待评审
> **日期**: 2026-07-08
> **范围**: Build Verification Test（构建验证测试）套件设计
> **定位**: L0 烟雾测试，构建后第一时间验证"构建是否可用"，失败则拒绝构建

---

## 0. BVT 定义

### 0.1 什么是 BVT

BVT（Build Verification Test）是构建后运行的**最小验证集**，用最快速度回答一个问题：

> **"这个构建是否值得进入下一轮测试？"**

BVT 不验证业务正确性，只验证**核心链路是否跑通**。BVT 失败意味着构建本身有根本性问题，不需要跑后续完整测试。

### 0.2 BVT 在测试金字塔中的位置

```
L4  Performance       nightly           基准 + 回归阈值
L3  Scenario          PR to 3.0-dev     跨服务 E2E 链路
L2  Functional        每 PR             每服务 API + 错误路径
L1  BVT               每次构建          烟雾测试，核心链路冒烟
L0  Build             每次 push         编译 + 静态检查
```

### 0.3 BVT vs Functional 的区别

| 维度 | BVT | Functional |
|---|---|---|
| 目的 | 验证构建可用 | 验证业务正确性 |
| 用例数 | 15-20 个 | 170+ 个 |
| 运行时间 | < 2 分钟 | 5-10 分钟 |
| 覆盖深度 | 仅 happy path 主干 | happy + error + boundary |
| 失败后果 | 拒绝构建，短路所有后续 | 报告失败，不阻塞构建 |
| 触发 | 每次构建（含 push） | PR |
| 数据清理 | 每用例独立，不残留 | 可共享 fixture |

---

## 1. 选取原则

BVT 用例选取遵循 **"三最"原则**：

1. **最核心** - 失败则 IM 不可用的功能（auth、消息、会话）
2. **最快速** - 单用例 < 10 秒，总时长 < 2 分钟
3. **最稳定** - 不依赖边界条件、不测竞态、不测性能

**BVT 必须覆盖的 5 条命脉链路：**

```
1. 认证链路    注册 -> 登录 -> 带 token 调 API
2. 社交链路    加好友 -> 通过 -> 成为好友
3. 消息链路    发消息 -> 同步 -> 读历史
4. 会话链路    建群 -> 加成员 -> 列会话
5. 媒体链路    申请上传 -> 完成 -> 下载
```

**BVT 明确不测的内容：**
- ❌ 错误路径（错误码/边界/权限）
- ❌ 并发/竞态
- ❌ 数据一致性深查（DB/ES 直查）
- ❌ WebSocket 推送（耗时且不稳定）
- ❌ 性能
- ❌ 安全注入

---

## 2. BVT 用例清单（18 个）

### 2.1 基础设施健康（3 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-001 | `TestBVT_GatewayHTTP_Reachable` | GET gateway:9000/health 返回 200 | 1s |
| BVT-002 | `TestBVT_GatewayWS_Reachable` | WS 连接 gateway:9001 成功 open | 1s |
| BVT-003 | `TestBVT_ServicesRegistered` | etcd 查询 8 个服务均有注册实例 | 2s |

### 2.2 认证链路（3 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-004 | `TestBVT_Register_Success` | 用户名注册成功，返回 user_id | 3s |
| BVT-005 | `TestBVT_Login_Success` | 登录成功，返回 access_token + refresh_token | 3s |
| BVT-006 | `TestBVT_AuthenticatedAPICall` | 带 token 调 GetProfile，返回自身信息 | 2s |

### 2.3 社交链路（2 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-007 | `TestBVT_SendFriendRequest_Success` | A 向 B 发好友申请，返回 notify_event_id | 3s |
| BVT-008 | `TestBVT_AcceptFriend_Success` | B 通过申请，返回 new_conversation_id | 3s |

### 2.4 消息链路（3 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-009 | `TestBVT_SendTextMessage_Success` | 发文本消息，返回 message_id + seq_id | 3s |
| BVT-010 | `TestBVT_SyncMessages_Success` | SyncMessages 返回刚发的消息 | 3s |
| BVT-011 | `TestBVT_GetHistory_Success` | GetHistory 返回消息列表 | 3s |

### 2.5 会话链路（3 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-012 | `TestBVT_CreateGroupConversation_Success` | 创建群会话，返回 conversation_id | 3s |
| BVT-013 | `TestBVT_AddMembers_Success` | 添加成员到群会话 | 3s |
| BVT-014 | `TestBVT_ListConversations_Success` | 列出会话，包含刚建的群 | 3s |

### 2.6 媒体链路（3 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-015 | `TestBVT_ApplyUpload_Success` | 申请上传，返回 file_id + upload_url | 2s |
| BVT-016 | `TestBVT_CompleteUpload_Success` | PUT 到 MinIO + CompleteUpload，success=true | 5s |
| BVT-017 | `TestBVT_ApplyDownload_Success` | 申请下载，返回 download_url，内容匹配 | 3s |

### 2.7 Presence 链路（1 个）

| ID | 名称 | 验证点 | 预计耗时 |
|---|---|---|---|
| BVT-018 | `TestBVT_GetPresence_Success` | 查询在线状态，返回 online | 2s |

### 2.8 统计

- 总用例：18 个
- 预计总耗时：~45 秒（含服务调用开销）
- 含 Docker 启动等待：CI 中总 ~3 分钟

---

## 3. BVT 用例详细设计

### 3.1 基础设施健康

#### BVT-001: TestBVT_GatewayHTTP_Reachable

```go
//go:build bvt

func TestBVT_GatewayHTTP_Reachable(t *testing.T) {
    resp, err := http.Get("http://127.0.0.1:9000/health")
    require.NoError(t, err)
    defer resp.Body.Close()
    assert.Equal(t, 200, resp.StatusCode)
}
```

**验证点**：gateway HTTP 端口存活。
**失败含义**：gateway 未启动或 crash，构建不可用。

#### BVT-002: TestBVT_GatewayWS_Reachable

```go
func TestBVT_GatewayWS_Reachable(t *testing.T) {
    ws, err := wsclient.Connect("ws://127.0.0.1:9001")
    require.NoError(t, err)
    defer ws.Close()
    assert.True(t, ws.IsConnected())
}
```

**验证点**：gateway WS 端口可连接。
**失败含义**：WS 服务未启动，实时推送不可用。

#### BVT-003: TestBVT_ServicesRegistered

```go
func TestBVT_ServicesRegistered(t *testing.T) {
    services := []string{"Service/identity", "Service/media", "Service/message",
        "Service/transmite", "Service/relationship", "Service/conversation",
        "Service/presence", "Service/push"}
    for _, svc := range services {
        instances, err := etcdClient.Get(svc)
        require.NoError(t, err)
        assert.NotEmpty(t, instances, "服务 %s 未注册", svc)
    }
}
```

**验证点**：8 个服务均注册到 etcd。
**失败含义**：某服务启动失败或注册逻辑 broken。

### 3.2 认证链路

#### BVT-004 ~ 006: 注册 -> 登录 -> 鉴权调用

```go
func TestBVT_AuthFlow(t *testing.T) {
    // BVT-004: 注册
    username := "bvt_" + client.NewRequestID()[:8]
    password := "Bvt@123456"
    regReq := &identity.RegisterReq{...}
    regRsp := &identity.RegisterRsp{}
    require.NoError(t, HTTP.DoNoAuth("/service/identity/register", regReq, regRsp))
    assert.True(t, regRsp.Header.Success)

    // BVT-005: 登录
    loginReq := &identity.LoginReq{Username: username, Password: password, ...}
    loginRsp := &identity.LoginRsp{}
    require.NoError(t, HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp))
    assert.True(t, loginRsp.Header.Success)
    assert.NotEmpty(t, loginRsp.AccessToken)

    // BVT-006: 鉴权调用
    HTTP.AccessToken = loginRsp.AccessToken
    profileReq := &identity.GetProfileReq{...}
    profileRsp := &identity.GetProfileRsp{}
    require.NoError(t, HTTP.DoAuth("/service/identity/get_profile", profileReq, profileRsp))
    assert.True(t, profileRsp.Header.Success)
}
```

> 注：BVT 中认证链路 3 步可合并为一个 test（共享 fixture），减少重复注册。实际实现时 BVT-004/005/006 可作为独立 test 或合并为 `TestBVT_AuthFlow`，取决于是否需要独立报告。

**验证点**：注册 -> 登录 -> 拿 token -> 带 token 调 API 全链路通。
**失败含义**：认证体系 broken，任何功能都不可用。

### 3.3 社交链路

#### BVT-007 ~ 008: 好友申请 -> 通过

```go
func TestBVT_FriendFlow(t *testing.T) {
    alice := fixture.RegisterAndLogin(t, HTTP)
    bob := fixture.RegisterAndLogin(t, HTTP)

    // BVT-007: 发好友申请
    sendReq := &relationship.SendFriendReq{RespondentId: bob.UserID, ...}
    sendRsp := &relationship.SendFriendRsp{}
    require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
    assert.True(t, sendRsp.Header.Success)

    // BVT-008: 通过申请
    handleReq := &relationship.HandleFriendReq{
        NotifyEventId: sendRsp.NotifyEventId, Agree: true, ApplyUserId: alice.UserID, ...}
    handleRsp := &relationship.HandleFriendRsp{}
    require.NoError(t, bob.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
    assert.True(t, handleRsp.Header.Success)
    assert.NotEmpty(t, handleRsp.NewConversationId)
}
```

**验证点**：好友关系建立 + 自动创建单聊会话。
**失败含义**：社交链路 broken，无法建立会话。

### 3.4 消息链路

#### BVT-009 ~ 011: 发消息 -> 同步 -> 历史

```go
func TestBVT_MessageFlow(t *testing.T) {
    alice, bob, convID := fixture.MakeFriends(t, HTTP) // 复用社交链路

    // BVT-009: 发文本消息
    sendReq := &transmite.SendMessageReq{
        ConversationId: convID,
        Content: &msg.MessageContent{
            Type: msg.MessageType_TEXT,
            Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "bvt hello"}},
        },
        ClientMsgId: client.NewRequestID(), ...
    }
    sendRsp := &transmite.SendMessageRsp{}
    require.NoError(t, alice.DoAuth("/service/transmite/send", sendReq, sendRsp))
    assert.True(t, sendRsp.Header.Success)
    assert.NotZero(t, sendRsp.Message.MessageId)

    // BVT-010: 同步消息
    syncReq := &msg.SyncMessagesReq{ConversationId: convID, AfterSeq: 0, Limit: 10, ...}
    syncRsp := &msg.SyncMessagesRsp{}
    require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
    assert.True(t, syncRsp.Header.Success)
    assert.NotEmpty(t, syncRsp.Messages)

    // BVT-011: 读历史
    histReq := &msg.GetHistoryReq{ConversationId: convID, BeforeSeq: syncRsp.LatestSeq + 1, Limit: 10, ...}
    histRsp := &msg.GetHistoryRsp{}
    require.NoError(t, bob.DoAuth("/service/message/get_history", histReq, histRsp))
    assert.True(t, histRsp.Header.Success)
    assert.NotEmpty(t, histRsp.Messages)
}
```

**验证点**：消息发送 -> MQ 投递 -> DB 落库 -> 同步拉取 -> 历史查询全链路通。
**失败含义**：IM 核心功能 broken，消息收发不可用。

### 3.5 会话链路

#### BVT-012 ~ 014: 建群 -> 加成员 -> 列会话

```go
func TestBVT_ConversationFlow(t *testing.T) {
    owner := fixture.RegisterAndLogin(t, HTTP)
    m1 := fixture.RegisterAndLogin(t, HTTP)
    m2 := fixture.RegisterAndLogin(t, HTTP)

    // BVT-012: 创建群会话
    name := "bvt-group"
    createReq := &conversation.CreateConversationReq{
        Type: conversation.ConversationType_GROUP, Name: &name,
        MemberIds: []string{m1.UserID, m2.UserID}, ...
    }
    createRsp := &conversation.CreateConversationRsp{}
    require.NoError(t, owner.DoAuth("/service/conversation/create", createReq, createRsp))
    assert.True(t, createRsp.Header.Success)
    convID := createRsp.Conversation.ConversationId

    // BVT-013: 添加成员（加第三人）
    m3 := fixture.RegisterAndLogin(t, HTTP)
    addReq := &conversation.AddMembersReq{ConversationId: convID, MemberIds: []string{m3.UserID}, ...}
    addRsp := &conversation.AddMembersRsp{}
    require.NoError(t, owner.DoAuth("/service/conversation/add_members", addReq, addRsp))
    assert.True(t, addRsp.Header.Success)

    // BVT-014: 列会话
    listReq := &conversation.ListConversationsReq{...}
    listRsp := &conversation.ListConversationsRsp{}
    require.NoError(t, owner.DoAuth("/service/conversation/list", listReq, listRsp))
    assert.True(t, listRsp.Header.Success)
    assert.NotEmpty(t, listRsp.Conversations)
}
```

**验证点**：群会话创建 -> 成员管理 -> 列表查询。
**失败含义**：会话管理 broken，群聊不可用。

### 3.6 媒体链路

#### BVT-015 ~ 017: 申请上传 -> 完成 -> 下载

```go
func TestBVT_MediaFlow(t *testing.T) {
    user := fixture.RegisterAndLogin(t, HTTP)
    content := []byte("bvt test content")
    hash := sha256.Sum256(content)

    // BVT-015: 申请上传
    applyReq := &media.ApplyUploadReq{
        FileName: "bvt.txt", FileSize: int64(len(content)),
        MimeType: "text/plain", ContentHash: fmt.Sprintf("sha256:%x", hash),
        Purpose: media.MediaPurpose_CHAT, ...
    }
    applyRsp := &media.ApplyUploadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
    assert.True(t, applyRsp.Header.Success)
    fileID := applyRsp.FileId

    // BVT-016: PUT 到 MinIO + CompleteUpload
    req, _ := http.NewRequest("PUT", applyRsp.UploadUrl, bytes.NewReader(content))
    resp, err := http.DefaultClient.Do(req)
    require.NoError(t, err)
    require.Equal(t, 200, resp.StatusCode)

    completeReq := &media.CompleteUploadReq{FileId: fileID, ...}
    completeRsp := &media.CompleteUploadRsp{}
    require.NoError(t, user.DoAuth("/service/media/complete_upload", completeReq, completeRsp))
    assert.True(t, completeRsp.Header.Success)

    // BVT-017: 申请下载并验证内容
    dlReq := &media.ApplyDownloadReq{FileId: fileID, ...}
    dlRsp := &media.ApplyDownloadRsp{}
    require.NoError(t, user.DoAuth("/service/media/apply_download", dlReq, dlRsp))
    assert.True(t, dlRsp.Header.Success)

    dlResp, err := http.Get(dlRsp.DownloadUrl)
    require.NoError(t, err)
    body, _ := io.ReadAll(dlResp.Body)
    assert.Equal(t, content, body)
}
```

**验证点**：三步上传全链路 + MinIO 对象读写 + 下载内容一致性。
**失败含义**：文件传输 broken，无法分享文件/图片/语音。

### 3.7 Presence 链路

#### BVT-018: 在线状态查询

```go
func TestBVT_PresenceQuery(t *testing.T) {
    user := fixture.RegisterAndLogin(t, HTTP) // 登录即在线
    req := &presence.GetPresenceReq{UserId: user.UserID, ...}
    rsp := &presence.GetPresenceRsp{}
    require.NoError(t, user.DoAuth("/service/presence/get", req, rsp))
    assert.True(t, rsp.Header.Success)
    assert.True(t, rsp.Online)
}
```

**验证点**：登录后 presence 为 online。
**失败含义**：在线状态服务 broken，影响消息推送路由。

---

## 4. 实现方式

### 4.1 目录结构

```
tests/
├── bvt/                          # BVT 测试（新增）
│   ├── setup_test.go             # TestMain + fixture
│   ├── health_test.go            # BVT-001 ~ 003
│   ├── auth_test.go              # BVT-004 ~ 006
│   ├── friend_test.go            # BVT-007 ~ 008
│   ├── message_test.go           # BVT-009 ~ 011
│   ├── conversation_test.go      # BVT-012 ~ 014
│   ├── media_test.go             # BVT-015 ~ 017
│   └── presence_test.go          # BVT-018
├── func/                         # 现有功能测试
├── perf/                         # 现有性能测试
├── pkg/                          # 共享包
└── Makefile
```

### 4.2 Build Tag

BVT 使用独立 build tag `//go:build bvt`，与 func/perf 隔离：

```go
//go:build bvt

package bvt_test

import (
    "testing"
    ...
)
```

### 4.3 Makefile 目标

在 `tests/Makefile` 新增：

```makefile
.PHONY: proto test-bvt test-func test-scenario test-perf clean deps

# ... 现有 proto/deps/clean 不变 ...

# Run BVT smoke tests (L0) - fastest, runs first
test-bvt:
	go test -tags=bvt ./bvt/... -v -count=1 -timeout=300s

# Run all functional tests (L2)
test-func:
	go test -tags=func ./func/... -v -count=1

# Run scenario tests only (L3)
test-scenario:
	go test -tags=func ./func/... -run TestScenario -v -count=1

# Run performance benchmarks (L4)
test-perf:
	go test -tags=perf ./perf/... -bench=. -benchmem -count=3 -benchtime=10s
```

### 4.4 CI 集成

CI workflow 新增 `bvt` job 作为最前置门控：

```yaml
jobs:
  bvt:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: sudo apt-get install -y protobuf-compiler netcat-openbsd
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto && go mod download
      - run: cd tests && make test-bvt
      - if: always()
        run: docker compose down -v

  func:
    needs: bvt          # BVT 失败则 func 不跑
    # ... 现有 func job ...

  scenario:
    needs: func
    # ... 现有 scenario job ...

  perf:
    needs: scenario
    # ... 现有 perf job ...
```

**CI 执行流：**

```
push/PR
  │
  ▼
bvt (~3min, 含 docker 启动)
  │ fail -> 拒绝构建，短路
  │ pass
  ▼
func (~5min)
  │ fail -> 报告失败
  │ pass
  ▼ (PR to 3.0-dev only)
scenario (~10min)
  │
  ▼ (nightly only)
perf (~15min)
```

### 4.5 本地运行

```bash
# 快速跑 BVT（开发中最常用）
docker compose up -d
cd tests && make test-bvt

# 跑完整功能测试
cd tests && make test-func
```

开发者本地改动后，先跑 BVT（~3min）确认构建没坏，再跑 func。

---

## 5. BVT 失败处理流程

### 5.1 失败分类

| 失败类型 | 典型表现 | 处理 |
|---|---|---|
| 基础设施未就绪 | BVT-001/002/003 fail | 检查 docker-compose、entrypoint.sh、etcd 注册 |
| 认证 broken | BVT-004/005/006 fail | 检查 identity 服务、JWT 签发、gateway 鉴权中间件 |
| 消息链路 broken | BVT-009/010/011 fail | 检查 transmite 服务、MQ 连通性、message 服务消费 |
| 构建编译错误 | go test 编译失败 | 检查 proto 生成、Go 依赖、import 路径 |

### 5.2 短路策略

BVT job 在 CI 中用 `needs` 串联，BVT 失败时 func/scenario/perf 均不执行：

- 节省 CI 资源（避免跑了 15min 才发现构建根本不可用）
- 快速反馈（开发者 ~3min 内知道构建是否可用）

### 5.3 Flaky 处理

BVT 要求**零 flaky**。如果某 BVT 用例不稳定：

1. **立即修复** - BVT 不容忍 flaky，要么修要么移出 BVT
2. **不加 retry** - BVT 失败不重试，retry 会掩盖真实问题
3. **降级到 func** - 如果某用例 inherently 不稳定，移到 func 套件

---

## 6. BVT 维护规则

### 6.1 新增 BVT 用例的条件

新增 BVT 用例需满足**全部**条件：
1. **核心链路** - 失败则 IM 不可用
2. **快速稳定** - 单用例 < 10s，无 flaky 历史
3. **不可被现有 BVT 覆盖** - 不重复验证已覆盖的链路

### 6.2 BVT 上限

- **硬上限 25 个** - 超过则总时长 > 2min，失去"快"的意义
- 当前 18 个，有 7 个余量供未来核心功能（如新服务）加入

### 6.3 从 BVT 移除用例的条件

1. 对应功能被废弃
2. 用例持续 flaky 且无法修复
3. 用例耗时 > 15s（应优化或降级到 func）

---

## 7. 统计

| 维度 | 数值 |
|---|---|
| BVT 用例总数 | 18 |
| 预计总耗时（纯测试） | ~45s |
| 预计 CI 总耗时（含 docker） | ~3min |
| 覆盖链路 | 5 条命脉（认证/社交/消息/会话/媒体）+ presence |
| 覆盖服务 | 8 个（identity/relationship/conversation/message/transmite/media/presence/gateway） |
| Build tag | `//go:build bvt` |
| Makefile 目标 | `make test-bvt` |
| CI job | `bvt`（最前置，func 的 needs 依赖） |

---

## 8. 与其他测试文档的关系

| 文档 | 定位 | 关系 |
|---|---|---|
| `2026-07-08-go-testing-design.md` | 测试架构总设计 | BVT 是 L0 层，本文档细化 |
| `2026-07-08-test-case-catalog.md` | L2/L3/L4 用例目录 | BVT 不在目录中，BVT 是独立子集 |
| `2026-07-08-phase0-ci-infrastructure.md` | Phase 0 CI 实施 | Phase 0 CI 应包含 BVT job |
| 本文档 | BVT 专项设计 | 独立设计，Phase 0 实现时落地 |

---

## 9. Phase 归属

BVT 套件应在 **Phase 0**（CI 基础设施）中落地，因为它是最前置的 CI 门控：

| Phase | 交付物 | 说明 |
|---|---|---|
| Phase 0 | `tests/bvt/` 18 个用例 + `make test-bvt` + CI bvt job | BVT 是 CI 的第一道门 |

Phase 0 的 plan（`2026-07-08-phase0-ci-infrastructure.md`）应更新，将 BVT 纳入 Task 范围。
