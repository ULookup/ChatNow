# ChatNow 测试架构主设计

> **状态**: 设计完成，待评审
> **日期**: 2026-07-09
> **范围**: ChatNow 全栈测试统一架构 - 5 层金字塔 / 4 目录 + 4 build tag / CI 5 job / 测试隔离 / 基础设施包 / 用例 ID 方案 / 分期实施
> **取代**: `archive/2026-07-08-go-testing-design.md`、`archive/2026-07-08-bvt-test-design.md`、`archive/2026-07-08-e2e-test-design.md`、`archive/2026-07-08-test-case-catalog.md`（已归档，保留为历史参考）
> **基线**: 3.0-dev 现有 Go 测试套件（tests/func/ 10 文件 / tests/perf/ 5 文件 / tests/pkg/ 5 文件）

---

## 0. 范围与原则

### 0.1 范围

本主 spec 统一 ChatNow 测试架构设计，覆盖：

- 5 层测试金字塔（L0 Build / L1 BVT / L2 Func / L3 Scenario / L4 Perf + Reliability）
- 目录结构与 build tag 策略
- CI 工作流（5 job 串联 + nightly）
- 测试隔离与全量清理
- 基础设施包设计（client/ws、verify/、cleanup/、chaos/、fixture/）
- 统一用例 ID 方案
- 用例目录摘要（244 用例，67 P0 / 81 P1 / 29 P2 + BVT 18 + others）
- 分期实施（Phase 0-3）

**不在范围**：Phase 1/2/3 的详细实现计划（由后续 writing-plans 产出）。

### 0.2 设计原则

前 5 条继承自原 go-testing-design，后 4 条为本主 spec 新增：

1. **纯 Go 测试** - 所有测试用 Go 编写，不保留 C++ gtest 测试。C++ 侧仅保留生产代码。
2. **黑盒行为测试** - 通过 HTTP + protobuf 外部 API 验证服务行为，不测 C++ 内部实现。
3. **复用现有设施** - tests/func/、tests/perf/、tests/pkg/ 已建立，在其上扩展而非另起炉灶。
4. **CI 驱动** - 测试必须能在 GitHub Actions 上自动跑，本地与 CI 命令一致。
5. **YAGNI** - 不引入额外测试框架，用 testify + Go 标准 testing。不 mock 服务，用真实全栈。
6. **分层金字塔 + BVT 门禁** - L1 BVT 失败短路 L2+，构建不可用时不浪费 L2/L3/L4 资源。
7. **每 run 全量清理** - TestMain 启动时 TRUNCATE + flush Redis + clear ES + clear MinIO，保证确定性状态。
8. **可靠性测试隔离** - `reliability` build tag + nightly only，不进 PR 流水线，独立 docker-compose stack。
9. **测试代码即权威** - 详细用例清单以测试函数名 + ID 注释为准，主 spec 只保留统计摘要与 P0 ID 列表。

明确**不做**的事：
- ❌ 不保留 C++ gtest 单元测试（common/test/、media/test/ 全部移除）
- ❌ 不做 C++ 接口抽取（Go 黑盒测试不需要改生产代码）
- ❌ 不在 macOS runner 上跑 CI（目标环境是 Linux）
- ❌ 不引入 testcontainers（用 docker-compose 更简单）
- ❌ 不单独搞"仅基础设施"的 compose（Go 功能测试需要业务服务在线）
- ❌ 不为每个测试用例维护独立文档（测试代码即权威）

---

## 1. 测试金字塔（5 层）

```
L4  Performance    nightly            基准 + 回归阈值        tests/perf/         perf tag
L3  Scenario       PR to 3.0-dev      跨服务 E2E + 一致性    tests/func/         func tag
L2  Functional     每 PR              每服务 API + 错误路径  tests/func/         func tag
L1  BVT            每次构建            烟雾测试，核心链路冒烟 tests/bvt/          bvt tag
L0  Build          每 push            编译 + 静态检查        (CI step)           -
```

### 1.1 层间边界

| 边界 | 区分标准 |
|---|---|
| L0 vs L1 | L0 验证"能编译"（cmake build + go vet + gofmt）；L1 验证"能跑起来"（5 条命脉链路 happy path） |
| L1 vs L2 | L1 只测 5 条命脉链路的 happy path（< 2min，18 用例）；L2 覆盖所有 API + 错误路径 + 边界 |
| L2 vs L3 | L2 测 1-2 个 API 调用；L3 测 5+ API 串联 + DB/ES/MinIO 直查一致性 |
| L3 vs L4 | L3 验证正确性；L4 测吞吐/延迟 |
| L4 vs Reliability | L4 测性能基线；Reliability 测故障恢复（MQ/服务/DB 重启不丢消息） |

### 1.2 每层用例数目标

| 层 | 现有 | 新增 | 合计 |
|---|---|---|---|
| L1 BVT | 0 | 18 | 18 |
| L2 Func（per-service） | 102 | 83 | 185 |
| L2 Func（cross-cutting：WS/DC/CC/SEC/QT） | 0 | 29 | 29 |
| L3 Scenario | 3 | 9 | 12 |
| L4 Perf | 5 | 3 | 8 |
| Reliability | 0 | 4 | 4 |
| **合计** | **110** | **146** | **256** |

> 注：合计 256 与归档 catalog 的 244 略有差异 - 本主 spec 把 SC-09~12（E2E spec 新增的 4 个场景）计入 L3 新增，catalog 原口径只算到 SC-08。

### 1.3 5 条命脉链路（BVT 必须覆盖）

```
1. 认证链路    注册 -> 登录 -> 带 token 调 API
2. 社交链路    加好友 -> 通过 -> 成为好友
3. 消息链路    发消息 -> 同步 -> 读历史
4. 会话链路    建群 -> 加成员 -> 列会话
5. 媒体链路    申请上传 -> 完成 -> 下载
```

---

## 2. 目录与 Build Tag 结构

### 2.1 目录结构

```
tests/
├── bvt/                    # //go:build bvt
│   ├── setup_test.go       # TestMain: cleanup + wait
│   ├── health_test.go      # BVT-001~003 基础设施健康
│   ├── auth_test.go        # BVT-004~006 认证链路
│   ├── social_test.go      # BVT-007~008 社交链路
│   ├── message_test.go     # BVT-009~011 消息链路
│   ├── conversation_test.go # BVT-012~014 会话链路
│   ├── media_test.go       # BVT-015~017 媒体链路
│   └── presence_test.go    # BVT-018 presence 链路
├── func/                   # //go:build func（含 L2 + L3）
│   ├── setup_test.go       # TestMain: cleanup + wait
│   ├── identity_test.go    # FN-ID-*
│   ├── relationship_test.go # FN-RL-*
│   ├── conversation_test.go # FN-CV-*
│   ├── message_test.go     # FN-MS-*
│   ├── transmite_test.go   # FN-TM-*
│   ├── media_test.go       # FN-MD-*
│   ├── presence_test.go    # FN-PR-*
│   ├── auth_middleware_test.go # FN-AM-*
│   ├── ws_notify_test.go   # FN-WS-* （新增）
│   ├── consistency_test.go # FN-DC-* （新增）
│   ├── concurrency_test.go # FN-CC-* （新增）
│   ├── security_test.go    # FN-SEC-*（新增）
│   ├── quota_test.go       # FN-QT-* （新增）
│   └── scenarios_test.go   # SC-* （L3）
├── perf/                   # //go:build perf
│   ├── setup_test.go
│   ├── login_test.go
│   ├── send_msg_test.go
│   ├── sync_test.go
│   ├── upload_test.go
│   ├── group_fanout_test.go  # PF-01（新增）
│   ├── media_upload_test.go  # PF-02（新增）
│   └── search_test.go        # PF-03（新增）
├── reliability/            # //go:build reliability
│   ├── setup_test.go
│   ├── mq_restart_test.go    # RL-01
│   ├── service_restart_test.go # RL-02
│   ├── db_reconnect_test.go  # RL-03
│   └── dead_letter_test.go   # RL-04
├── pkg/                    # 无 tag（被各层引用）
│   ├── client/{http,config,ws}.go
│   ├── fixture/{auth,friend,conversation,group,message,media,ws}.go
│   ├── verify/{db,es,minio}.go
│   ├── cleanup/cleanup.go
│   └── chaos/docker.go
├── proto/                  # gitignore（make proto 生成）
├── Makefile
├── config.yaml
├── go.mod
└── go.sum
```

### 2.2 Build Tag 规则

- 每个测试文件首行 `//go:build <tag>` + 空行 + `package <pkg>`
- `setup_test.go` 也带 tag（TestMain 在带 tag 的包内运行）
- `tests/pkg/` 下辅助包**不带 tag**（被各层共享引用）
- L3 scenario 与 L2 func 共用 `func` tag，scenario 通过 `-run TestScenario` 过滤

### 2.3 Makefile 目标

```makefile
.PHONY: proto test-bvt test-func test-scenario test-perf test-reliability clean deps

proto:
	# 生成 Go protobuf（现有逻辑保留）

test-bvt:
	go test -tags=bvt ./bvt/... -v -count=1

test-func:
	go test -tags=func ./func/... -v -count=1

test-scenario:
	go test -tags=func ./func/... -run TestScenario -v -count=1

test-perf:
	go test -tags=perf ./perf/... -bench=. -benchmem -count=3 -benchtime=10s

test-reliability:
	go test -tags=reliability ./reliability/... -v -count=1

deps:
	go mod download && go mod tidy

clean:
	rm -rf proto/chatnow
```

---

## 3. CI 工作流

### 3.1 5 个 Job

| Job | 层 | 触发 | 依赖 | 内容 | 预计耗时 |
|---|---|---|---|---|---|
| `build` | L0 | push + PR + nightly | - | cmake build + go vet + gofmt + go mod tidy check | 3-5min |
| `bvt` | L1 | push（非 main）+ PR + nightly | build | docker compose up + wait + `make test-bvt`（失败短路） | 4-5min |
| `func` | L2+L3 | PR + nightly | bvt | `make test-func`（含 scenario） | 8-12min |
| `perf` | L4 | nightly only | func | `make test-perf` | 15-20min |
| `reliability` | - | nightly only | func | `make test-reliability`（独立 docker-compose stack） | 10-15min |

### 3.2 依赖图

```
push     ─▶ build
PR       ─▶ build ─▶ bvt ─▶ func
nightly  ─▶ build ─▶ bvt ─▶ func ─▶ perf
                                   └─▶ reliability
```

### 3.3 关键设计

- **BVT 门禁**：`bvt` job 失败时 `func` 不跑（短路），节省 CI 资源
- **可靠性隔离**：`reliability` 用独立 docker-compose stack（因要 stop/start 中间件，不能影响其他 job）
- **func 含 scenario**：`func` job 内部跑全量 L2 + L3，`test-scenario` 仅作本地快捷目标
- **每 job 收尾**：`docker compose down -v`，保证不残留
- **Stack 复用优化**：`bvt` 跑完后 `func` 可复用同一 stack（不 down -v），仅重新 `CleanupAll`；或 CI 用 job cache

### 3.4 与原 go-testing-design 的差异

原设计 3 job（func/scenario/perf），本主 spec 升级为 5 job：
- 新增 `bvt` job（L1 门禁，在 func 之前）
- 新增 `reliability` job（nightly，独立 stack）
- `scenario` 不再独立 job（合并入 func，用 `-run` 过滤）

### 3.5 Workflow YAML 骨架

```yaml
name: CI
on:
  push:
    branches: [main, develop, 3.0-dev]
  pull_request:
    branches: [3.0-dev]
  schedule:
    - cron: "0 2 * * *"   # nightly

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: mkdir build && cd build && cmake .. && cmake --build . -j$(nproc)
      - run: go vet ./tests/...
      - run: gofmt -l tests/ | tee /dev/stderr | (! read)  # 非空则 fail

  bvt:
    needs: build
    runs-on: ubuntu-22.04
    if: github.event_name != 'push' || github.ref != 'refs/heads/main'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto && make test-bvt
      - if: always()
        run: docker compose down -v

  func:
    needs: bvt
    runs-on: ubuntu-22.04
    if: github.event_name == 'pull_request' || github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto && make test-func
      - if: always()
        run: docker compose down -v

  perf:
    needs: func
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto && make test-perf
      - if: always()
        run: docker compose down -v

  reliability:
    needs: func
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.23' }
      - run: docker compose up -d --build
      - run: ./scripts/wait_for_services.sh
      - run: cd tests && make proto && make test-reliability
      - if: always()
        run: docker compose down -v
```

---

## 4. 测试隔离与全量清理

### 4.1 清理范围

| 后端 | 清理操作 | 目标 |
|---|---|---|
| MySQL | `TRUNCATE TABLE` 各业务表 | 清空 message / user_timeline / conversation / conversation_member / friend / friend_request / user / media_blob_ref / media_user_quota / media_object / group 等 |
| Redis | `FLUSHDB`（仅 chatnow DB，非 FLUSHALL） | 清空所有 session / jwt blacklist / presence / typing 等 key |
| Elasticsearch | `DELETE chatnow_*` index pattern | 清空消息索引 |
| MinIO | 删除 chatnow bucket 下所有对象 | 清空媒体文件 |

**TRUNCATE 顺序**（按外键依赖反序）：
```
user_timeline -> message -> conversation_member -> conversation
-> friend_request -> friend -> user -> media_blob_ref -> media_user_quota -> media_object
```

### 4.2 清理包设计

`tests/pkg/cleanup/cleanup.go`：

```go
package cleanup

import (
    "testing"
    "time"
)

// CleanupAll 清空所有后端数据，保证测试 run 确定性状态。
// 失败时 t.Fatal（除 t=nil 时 panic）。
func CleanupAll(t testing.TB) {
    truncateMySQL(t)
    flushRedis(t)
    clearESIndices(t)
    clearMinIOBuckets(t)
}

// WaitForStackReady 轮询 gateway:9000/health + 8 个业务服务端口，
// 全部就绪后返回 nil，超时返回 error。
// 替代 wait_for_services.sh，本地与 CI 共用。
func WaitForStackReady(timeout time.Duration) error {
    // 轮询 127.0.0.1:9000/health + 10003/10002/10004/10005/10006/10007/10008
}
```

### 4.3 调用点

每个测试包的 `setup_test.go`：

```go
//go:build bvt

package bvt

import (
    "os"
    "testing"
    "time"

    "chatnow-tests/pkg/cleanup"
)

func TestMain(m *testing.M) {
    if err := cleanup.WaitForStackReady(120 * time.Second); err != nil {
        panic(err)
    }
    cleanup.CleanupAll(nil)
    os.Exit(m.Run())
}
```

`func`、`perf`、`reliability` 包的 `setup_test.go` 同构（替换 build tag 和 package 名）。

### 4.4 跨包协调

| 场景 | 策略 |
|---|---|
| `go test ./...`（默认） | Go 串行跑各 package，不会并发清理冲突 |
| CI 每个 package 独立 step | 前序完成后才跑下一个，天然串行 |
| 本地 `go test -p N`（并行） | 文件锁 `tests/.cleanup.lock` 串行化清理 |
| 包内并发（`t.Parallel()`） | 仅用于无状态依赖的测试；DB/Redis 操作的测试不并行 |

### 4.5 wait_for_services.sh 的去留

**保留**：作为 shell 脚本供 CI workflow 直接调用（在 `make test-*` 之前）。
**同时**：`cleanup.WaitForStackReady()` 供 Go 测试代码内部调用（如 reliability 测试中途等中间件恢复）。

两者逻辑等价，shell 实现供 CI 起栈时更早介入，Go 实现供测试代码复用。

---

## 5. 基础设施包设计

### 5.1 `tests/pkg/client/ws.go` - WebSocket 客户端

**职责**：连接 gateway WS、发送 auth 帧、读取 protobuf 通知帧、按类型分发。

**核心 API**：

```go
package client

type WSClient struct {
    conn        *websocket.Conn
    accessToken string
    userID      string
    deviceID    string
    notifies    []proto.Message
    notifyCh    chan proto.Message
    closed      bool
}

func NewWSClient(cfg *Config, accessToken, userID, deviceID string) (*WSClient, error)
func (w *WSClient) WaitForNotify(ctx context.Context, msgType string) (proto.Message, error)
func (w *WSClient) WaitForNotifyCount(ctx context.Context, msgType string, n int) ([]proto.Message, error)
func (w *WSClient) Close() error
```

**实现要点**：
- 连接后发 auth 帧（access_token + device_id），等 server ack
- `readLoop` 解析 protobuf 帧按 type 入 channel
- `WaitForNotify` 超时返回 error（默认 5s，可配）
- 测试结束 `Close()` 释放连接，避免 WS 连接泄漏

**依赖**：`github.com/gorilla/websocket` + `google.golang.org/protobuf`。

### 5.2 `tests/pkg/verify/` - 数据一致性验证

**职责**：直查 DB/ES/MinIO，验证 HTTP 响应与底层存储一致。

#### 5.2.1 `db.go`（MySQL 直查）

```go
package verify

type DBVerifier struct { db *sql.DB }

func NewDBVerifier(dsn string) *DBVerifier

func (v *DBVerifier) MessageExists(t testing.TB, messageID string)
func (v *DBVerifier) MessageCount(t testing.TB, convID string, expected int)
func (v *DBVerifier) UserTimelineExists(t testing.TB, userID, convID string, expected int)
func (v *DBVerifier) FriendRelationExists(t testing.TB, uidA, uidB string)
func (v *DBVerifier) UnreadCount(t testing.TB, userID, convID string, expected int)
func (v *DBVerifier) MessageStatus(t testing.TB, messageID string, expected int32)
func (v *DBVerifier) MediaQuota(t testing.TB, userID string, expected int64)
```

#### 5.2.2 `es.go`（ES 直查）

```go
type ESVerifier struct { client *http.Client; esURL string }

func NewESVerifier(url string) *ESVerifier
func (v *ESVerifier) MessageIndexed(t testing.TB, messageID, content string)
func (v *ESVerifier) SearchHitCount(t testing.TB, query string, expected int)
```

#### 5.2.3 `minio.go`（MinIO 直查）

```go
type MinIOVerifier struct { client *minio.Client }

func NewMinIOVerifier(endpoint, accessKey, secretKey string) *MinIOVerifier
func (v *MinIOVerifier) ObjectExists(t testing.TB, bucket, key string)
func (v *MinIOVerifier) ObjectContent(t testing.TB, bucket, key string, expected []byte)
func (v *MinIOVerifier) ObjectCount(t testing.TB, bucket string, expected int)
```

**依赖**：`github.com/go-sql-driver/mysql` + `github.com/minio/minio-go/v7` + 标准 net/http（ES）。

### 5.3 `tests/pkg/cleanup/` - 全量清理

见 §4.2。

### 5.4 `tests/pkg/chaos/` - Docker 控制（可靠性测试）

**职责**：封装 `docker compose` 命令，供 reliability 测试控制中间件。

```go
package chaos

func StopService(t testing.TB, name string) error
func StartService(t testing.TB, name string) error
func RestartService(t testing.TB, name string) error
func WaitServiceHealthy(t testing.TB, name string, timeout time.Duration) error
```

**实现**：`os/exec.Command("docker", "compose", "stop", name)` 等。

**注意**：仅 `reliability` tag 下测试使用；PR 流水线不跑这些测试，不影响其他 job。

### 5.5 `tests/pkg/fixture/` - 测试 Fixtures

**现有保留**：
- `auth.go`: `RegisterAndLogin(t) -> (userID, token)`
- `friend.go`: `EstablishFriendship(t, clientA, clientB) -> convID`
- `conversation.go`: `CreateSingleConversation(t, clientA, clientB) -> convID`

**新增**：

| 文件 | 函数 | 用途 |
|---|---|---|
| `group.go` | `CreateGroup(t, owner, members []User) -> convID` | 快速建群 |
| `group.go` | `AddMembers(t, owner, convID, users []User)` | 群加人 |
| `message.go` | `SendTextMessage(t, client, convID, text) -> msgID` | 快速发文本 |
| `message.go` | `SendImageMessage(t, client, convID, fileID) -> msgID` | 发图片消息 |
| `media.go` | `UploadFile(t, client, content, mime) -> fileID` | 完整三步上传 |
| `media.go` | `UploadLargeFile(t, client, content, mime, partSize) -> fileID` | 分片上传 |
| `ws.go` | `ConnectWS(t, client) -> *WSClient` | 建立 WS 连接 |

**设计原则**：
- Fixture 不做断言（除 fatal 错误如 `t.Fatal`）
- Fixture 返回关键 ID（user_id/conv_id/msg_id/file_id），测试代码基于 ID 做断言
- Fixture 内部用 `client.NewRequestID()` 生成唯一标识，保证多 run 不冲突

---

## 6. 用例 ID 方案

### 6.1 统一格式

`<LAYER>-<CATEGORY>-<NN>`，其中 LAYER 标识测试层，CATEGORY 标识服务或横切类别。

| LAYER 前缀 | 含义 | 示例 |
|---|---|---|
| `BVT` | L1 烟雾测试（无 CATEGORY，直接编号） | `BVT-001` ~ `BVT-018` |
| `FN-ID` | L2 identity 服务 | `FN-ID-01` ~ `FN-ID-12` |
| `FN-RL` | L2 relationship 服务 | `FN-RL-01` ~ `FN-RL-08` |
| `FN-CV` | L2 conversation 服务 | `FN-CV-01` ~ `FN-CV-10` |
| `FN-MS` | L2 message 服务 | `FN-MS-01` ~ `FN-MS-14` |
| `FN-TM` | L2 transmite 服务 | `FN-TM-01` ~ `FN-TM-08` |
| `FN-MD` | L2 media 服务 | `FN-MD-01` ~ `FN-MD-18` |
| `FN-PR` | L2 presence 服务 | `FN-PR-01` ~ `FN-PR-08` |
| `FN-AM` | L2 auth_middleware | `FN-AM-01` ~ `FN-AM-05` |
| `FN-WS` | L2 WebSocket 推送 | `FN-WS-01` ~ `FN-WS-07` |
| `FN-DC` | L2 数据一致性 | `FN-DC-01` ~ `FN-DC-07` |
| `FN-CC` | L2 并发 | `FN-CC-01` ~ `FN-CC-05` |
| `FN-SEC` | L2 安全 | `FN-SEC-01` ~ `FN-SEC-06` |
| `FN-QT` | L2 限流配额 | `FN-QT-01` ~ `FN-QT-04` |
| `SC` | L3 场景（无 CATEGORY，直接编号） | `SC-01` ~ `SC-12` |
| `PF` | L4 性能（无 CATEGORY） | `PF-01` ~ `PF-08` |
| `RL` | 可靠性（无 CATEGORY） | `RL-01` ~ `RL-04` |

### 6.2 测试函数命名

`Test<LAYER>_<Category>_<CaseName>`，例如：

- `TestBVT_Auth_RegisterSuccess`
- `TestFN_ID_LoginEmailSuccess`
- `TestFN_MS_RecallByNonAuthor`
- `TestSC_OfflineMessageSync`
- `TestPF_GroupMessageFanOut`
- `TestRL_MQRestart`

### 6.3 用例追踪

每个测试函数顶部加注释块，标注 ID/优先级/链路/验证点，便于反查：

```go
// FN-MS-06 | P0 | error path | 非发送者撤回消息应失败
func TestFN_MS_RecallByNonAuthor(t *testing.T) { ... }
```

详细用例清单不再单独维护（测试代码即权威）。归档的 `2026-07-08-test-case-catalog.md` 作为历史规划参考保留。

---

## 7. 用例目录摘要

### 7.1 按层 × 类别统计

| 层 × 类别 | 现有 | 新增 | 合计 | P0 | P1 | P2 |
|---|---|---|---|---|---|---|
| L1 BVT | 0 | 18 | 18 | 18 | 0 | 0 |
| L2 FN-ID（identity） | 19 | 12 | 31 | 6 | 12 | 4 |
| L2 FN-RL（relationship） | 15 | 8 | 23 | 2 | 6 | 4 |
| L2 FN-CV（conversation） | 22 | 10 | 32 | 3 | 9 | 4 |
| L2 FN-MS（message） | 14 | 14 | 28 | 6 | 8 | 3 |
| L2 FN-TM（transmite） | 14 | 8 | 22 | 4 | 5 | 2 |
| L2 FN-MD（media） | 6 | 18 | 24 | 8 | 9 | 3 |
| L2 FN-PR（presence） | 7 | 8 | 15 | 1 | 5 | 3 |
| L2 FN-AM（auth_middleware） | 5 | 5 | 10 | 2 | 2 | 1 |
| L2 FN-WS（WebSocket） | 0 | 7 | 7 | 2 | 4 | 1 |
| L2 FN-DC（一致性） | 0 | 7 | 7 | 3 | 4 | 0 |
| L2 FN-CC（并发） | 0 | 5 | 5 | 1 | 3 | 1 |
| L2 FN-SEC（安全） | 0 | 6 | 6 | 3 | 3 | 0 |
| L2 FN-QT（限流配额） | 0 | 4 | 4 | 2 | 1 | 1 |
| L3 SC（场景） | 3 | 9 | 12 | 5 | 6 | 1 |
| L4 PF（性能） | 5 | 3 | 8 | 0 | 2 | 1 |
| RL（可靠性） | 0 | 4 | 4 | 1 | 2 | 1 |
| **合计** | **110** | **146** | **256** | **67** | **81** | **29** |

> P0/P1/P2 合计 177 = 67+81+29。剩余 79 个为现有 happy path 用例（未单独标优先级，实现时按 P1 对待）。BVT 18 个全为 P0；PF 5 个现有基准未标优先级；RL 4 个均有优先级。

### 7.2 P0 用例清单（需在 Phase 1-2 完成或确认已覆盖）

以下列出 62 个关键 P0 用例（BVT 18 + L2 新增 38 + L3 5 + RL 1）。现有已实现的 happy path P0 用例（如 FN-ID Register/Login_Success、FN-TM SendMessage_Success 等）不在此重复列出，实现状态以测试代码为准。

**L1 BVT（18 个）**：BVT-001 ~ BVT-018（5 条命脉链路冒烟，详见归档 BVT spec）

**L2 P0（38 个，新增/补充用例）**：
- FN-ID: Login_Email_Success / Login_Email_InvalidCode / RefreshToken_Expired
- FN-RL: SendFriendRequest_Self
- FN-CV: RemoveMembers_LastOwner / TransferOwner_ToNonMember
- FN-MS: SelectByClientMsgId_Found / SelectByClientMsgId_NotFound / UpdateReadAck_Success / UpdateReadAck_Idempotent / SyncMessages_NotMember / RecallMessage_ByNonAuthor / DeleteMessages_NotOwned
- FN-TM: SendMessage_LargeGroup_ReadDiffusion / SendMessage_MQFailure / SendMessage_DismissedConversation
- FN-MD: CompleteUpload_Success / CompleteUpload_NotUploaded / InitMultipart_Success / ApplyPartUpload_Success / CompleteMultipart_FullFlow / ApplyUpload_Dedup_SameHash / ApplyUpload_QuotaExceeded / ApplyDownload_Success
- FN-PR: SubscribePresence_NotificationDelivery
- FN-AM: JWTRequired_MalformedToken / JWTRequired_WrongSignature
- FN-WS: NewMessageNotify / FriendRequestNotify
- FN-DC: MessageWriteDiffusion / ESIndexSync / UnreadCount
- FN-CC: SendMessage_SameClientMsgId
- FN-SEC: AuthBypass_NoToken / AuthBypass_OtherUser / PrivilegeEscalation_MemberToOwner
- FN-QT: MediaUpload_ExceedUserQuota / MediaUpload_ExceedSingleFile

**L3 P0（5 个）**：
- SC-01 RegisterToFirstMessage（注册->登录->加好友->发消息->sync->history 基础链路，已有）
- SC-04 OfflineMessageSync（离线消息不丢、按序、不重复）
- SC-05 MediaUploadFullFlow（三步上传 + 去重 + 分片 + 下载一致性）
- SC-06 MessageReliability（MQ 故障不丢消息 + client_msg_id 幂等）
- SC-09 UnreadCountConsistency（未读数跨服务跨设备一致）

**RL P0（1 个）**：RL-01 MQRestart（MQ 重启消息最终落库）

### 7.3 详细用例清单

不再单独维护。详细用例清单以测试代码为准（函数名 + ID 注释）。归档的 `2026-07-08-test-case-catalog.md` 作为历史规划参考保留。新增用例在实现时直接在代码中加 ID 注释，本主 spec 摘要表定期同步合计数。

---

## 8. 分期实施

### 8.1 Phase 0：CI 基础设施（已有 plan，进行中）

**已有 plan**：`docs/superpowers/plans/2026-07-08-phase0-ci-infrastructure.md`

**交付**：
- `scripts/wait_for_services.sh`（服务健康检查）
- `.github/workflows/ci.yml`（原 plan 三 job，本主 spec 修正为五 job）
- `tests/Makefile` 微调
- `tests/.gitignore`

**本主 spec 对 Phase 0 plan 的修正**：
- CI workflow 需新增 `bvt` job（在 func 之前）
- 新增 `reliability` job（nightly，独立 stack）
- 原 plan 的三 job 升级为五 job

**验收**：PR 触发 bvt + func job，现有 10 个 func 测试文件全绿。

### 8.2 Phase 1：BVT + 核心消息链路 + 横切基建（~67 用例）

**基建**：
- `tests/bvt/` 目录（18 用例）
- `tests/pkg/cleanup/`（全量清理）
- `tests/pkg/client/ws.go`（WebSocket 客户端）
- `tests/pkg/verify/{db,es}.go`（MySQL + ES 直查）
- `tests/pkg/fixture/{group,message,ws}.go`（新增 fixture）

**用例**：
- BVT：18 个全量
- L2 P0：transmite + message 错误路径 + 2 个未测 message API（SelectByClientMsgId / UpdateReadAck）+ 1 个未测 conversation API（GetMemberIds）
- L3 P0：SC-04 离线同步 / SC-06 消息可靠性（reliability tag 下另跑，scenario 下做"MQ 可用时"版本）
- 横切 P0：WS-01/02 + DC-01/02/03 + CC-01 + SEC-01/02/06

**CI 更新**：workflow 加 `bvt` job

**验收**：BVT 18 + L2 P0 ~30 + L3 P0 2 + 横切 P0 7 = ~57 用例；bvt job 门禁生效

### 8.3 Phase 2：media + presence + 安全 + C++ 移除（~67 用例）

**用例**：
- L2：FN-MD 18 + FN-PR 8 + FN-SEC 4（剩余）
- L3：SC-05 媒体全链路 / SC-07 多设备 / SC-08 大群读扩散 / SC-09 未读一致 / SC-10 撤回可见 / SC-11 token 刷新 / SC-12 ES 检索
- 横切：DC-04~07 + WS-03~07 + CC-02~05 + SEC-03~05

**基建**：
- `tests/pkg/verify/minio.go`（MinIO 直查）
- `tests/pkg/fixture/media.go`（媒体 fixture）

**清理**：
- 移除 `common/test/`（15 个 C++ 测试文件）
- 移除 `media/test/`（2 个 C++ 测试文件）
- 移除 `identity/test/`（如存在）
- 根 `CMakeLists.txt` 移除 `add_subdirectory(common/test)`

**验收**：L2 ~30 + L3 7 + 横切 ~15 + C++ 测试全删 = ~52 用例；CMake 不再构建 test target；Go 行为测试覆盖等价行为（对照归档 go-testing-design 的 C++->Go 映射表）

### 8.4 Phase 3：可靠性 + 限流配额 + 性能基线 + 边角（~26 用例）

**用例**：
- 可靠性：`tests/reliability/` 4 个 + `tests/pkg/chaos/`
- 限流配额：FN-QT 4 个
- 性能基线：PF-01/02/03 + 基线建立 + 回归阈值（吞吐下降 >10% fail）
- 边角 P2：各服务剩余 P2 用例

**CI 更新**：workflow 加 `perf` + `reliability` nightly job

**验收**：RL 4 + QT 4 + PF 3 + P2 ~15 = ~26 用例；nightly 全绿

---

## 9. 风险与缓解

| 风险 | 缓解 |
|---|---|
| Cleanup 30-60s 启动开销拖慢 CI | BVT job 跑完后 func job 复用同一 docker-compose stack（不 down -v），仅重新 CleanupAll；或 CI 用 job cache |
| TRUNCATE 跨 package 并发冲突 | Go 默认串行跑 package；CI 每 package 独立 step；本地 `-p` 并行时用文件锁 `tests/.cleanup.lock` |
| docker compose stop 中间件影响其他测试 | reliability 测试独立 docker-compose stack（compose 文件分离或独立 job） |
| WS 推送时序不稳定导致 flaky | `WaitForNotify` 带超时 + 重试；断言用"最终一致"（轮询 5s）而非"立即到达"；必要时关闭 `t.Parallel()` |
| 256 用例工作量大 | 按 Phase 拆分，P0 优先（67 个）；Phase 1 ~57 / Phase 2 ~52 / Phase 3 ~26，单 Phase 可 1-2 周完成 |
| C++ 测试移除丢失覆盖 | Phase 2 移除前对照归档 go-testing-design 的 C++->Go 行为映射表确认等价覆盖 |
| MinIO/ES 直查依赖内部 schema | verify 包断言失败时打印实际 schema 便于排查；schema 变更时 verify 包同步更新 |
| Build tag 错配导致测试漏跑 | Makefile 目标显式带 `-tags`；CI 每步 `go test -v` 输出可见哪些用例跑了 |
| 可靠性测试在 macOS 本地跑不了（docker compose 行为差异） | 文档注明 reliability 测试仅在 Linux CI 跑；本地 macOS 跳过 `reliability` tag |
| Go protobuf 生成依赖 protoc | Makefile 的 `make proto` 目标已处理；CI 装 protobuf-compiler |
| 现有 func 测试偏 happy path | Phase 1 重点补错误路径 |

---

## 10. 与归档 spec 的对应关系

| 归档 spec | 内容去向 |
|---|---|
| `archive/2026-07-08-go-testing-design.md` | §0 原则 / §1 金字塔 / §2 目录 / §3 CI（升级为五 job）/ §8 Phase 0-3 |
| `archive/2026-07-08-bvt-test-design.md` | §1.2 L1 层 / §2.1 tests/bvt/ 目录 / §3.1 bvt job / §7.2 BVT-001~018 P0 清单 |
| `archive/2026-07-08-e2e-test-design.md` | §1.2 L3 层 / §5.1 WS 客户端 / §5.2 verify 包 / §7.2 SC-04~12 P0 清单 |
| `archive/2026-07-08-test-case-catalog.md` | §6 ID 方案 / §7 用例目录摘要 / §8 Phase 1-3 用例分配 |

归档 spec 保留为历史参考，不再维护。本主 spec 为权威设计文档。

---

## 11. 总结

本主 spec 将 ChatNow 测试统一为：

1. **5 层金字塔** - L0 Build / L1 BVT / L2 Func / L3 Scenario / L4 Perf + Reliability，BVT 门禁短路
2. **4 目录 + 4 build tag** - tests/bvt/ (bvt) / tests/func/ (func) / tests/perf/ (perf) / tests/reliability/ (reliability)
3. **CI 5 job** - build -> bvt -> func -> perf + reliability（nightly）
4. **每 run 全量清理** - TestMain 调 cleanup.CleanupAll，保证确定性状态
5. **5 个基础设施包** - client/ws、verify/{db,es,minio}、cleanup、chaos、fixture 扩展
6. **统一 ID 方案** - `<LAYER>-<CATEGORY>-<NN>`，测试代码即权威
7. **256 用例** - 67 P0 / 81 P1 / 29 P2 + BVT 18 + PF 8 + RL 4 + 现有 49
8. **4 Phase 实施** - Phase 0 CI / Phase 1 BVT+核心 (~57) / Phase 2 media+presence+C++ 移除 (~52) / Phase 3 可靠性+perf (~26)

与原 4 份 spec 的根本区别：**单一权威设计**，消除重叠与不一致；补齐横切缺口（隔离/可靠性基建/fixture/ID）；CI 升级为五 job 含 BVT 门禁。
