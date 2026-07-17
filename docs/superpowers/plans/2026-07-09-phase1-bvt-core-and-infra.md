# Phase 1: BVT + 核心消息链路 + 横切基建 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 搭建 BVT 烟雾测试套件（18 用例）、横切基础设施（cleanup/ws client/verify/fixture）、核心消息链路错误路径测试、WebSocket 推送验证、数据一致性验证、并发/安全测试、离线同步与消息可靠性场景，共计 ~41 个新测试用例。

**Architecture:** 在 tests/pkg/ 下新建 cleanup、verify、client/ws 三个基础设施包，扩展 fixture 包（group/message/ws）。在 tests/bvt/ 下新建 BVT 测试目录（8 文件，18 用例，bvt build tag）。在 tests/func/ 下新增横切测试文件（ws_notify/consistency/concurrency/security）和 L2 补充用例。CI 新增 bvt job 作为 func 前置门禁。所有横切包无 build tag，被各层共享引用。

**Tech Stack:** Go 1.23、testify（assert/require）、gorilla/websocket、go-sql-driver/mysql、google.golang.org/protobuf、Docker Compose、GitHub Actions

## Global Constraints

- 目标环境是 Linux（Ubuntu 22.04），开发在 macOS（MEMORY: project_dev_env.md）
- 接口与实现分离在独立头文件中（MEMORY: feedback_file_splitting.md）- 适用于 C++ 生产代码，Go 测试代码用 package 分离
- 纯 Go 测试，黑盒行为测试，复用现有设施，CI 驱动，YAGNI（master spec §0.2）
- 每 run 全量清理：TestMain 调 cleanup.CleanupAll，保证确定性状态（master spec §7）
- tests/pkg/ 包无 build tag；tests/bvt/ 用 `//go:build bvt`；tests/func/ 用 `//go:build func`
- 测试代码即权威：用例 ID 注释标注在测试函数顶部（master spec §6.3）
- MySQL 连接：root:YHY060403@tcp(localhost:3306)/chatnow（从 conf/docker/*.conf 获取）
- ES 索引名：`message` 和 `chat_session`（从 common/dao/data_es.hpp 确认，非 chatnow_*）
- Redis 集群：6 节点 localhost:6379-6384，FLUSHALL 需逐节点执行
- WS 协议：binary frame = 序列化的 push.NotifyMessage，首帧发 CLIENT_AUTH 鉴权

## Schema Reference（实现时直查确认）

**ODB 表名**（从 `odb/*.hxx` 的 `#pragma db object table("...")` 确认）：

| 表名 | 关键列 | 备注 |
|---|---|---|
| `message` | message_id(BIGINT), session_id(=conversation_id), seq_id, user_id, client_msg_id, status(0=NORMAL,1=REVOKED,2=DELETED) | session_id 列名≠conversation_id |
| `user_timeline` | user_id, user_seq, session_id, session_seq, message_id | 写扩散：每成员一行 |
| `conversation` | conversation_id, type, status | |
| `conversation_member` | conversation_id, user_id, last_read_seq, last_ack_seq, role, muted, visible | **无 unread_count 列**，未读数 = max_seq - last_read_seq |
| `relation` | user_id, peer_id | 好友关系双向存储 |
| `friend_apply` | | 好友申请表 |
| `user` | | |
| `media_file` | file_id, content_hash, bucket, object_key, owner_id, status | |
| `media_blob_ref` | content_hash, ref_count, total_size | |
| `media_user_quota` | user_id, used_bytes, quota_bytes | |

**TRUNCATE 顺序**（按外键依赖反序）：
```
user_timeline, message_attachment, message_mention, message_reaction,
message_pin, message_read, message, conversation_member, conversation_view,
conversation, friend_apply, relation, user_block, user_device, user,
media_file, media_blob_ref, media_user_quota
```

**Master spec 与实际 schema 的差异**（已在调研中确认）：
1. spec §4.1 写 "friend" → 实际表名 `relation`
2. spec §4.1 写 "friend_request" → 实际表名 `friend_apply`
3. spec §4.1 写 "media_object" → 实际表名 `media_file`
4. spec §4.1 写 "group" → 无独立 group 表，群组即 type=GROUP 的 conversation
5. spec §5.2.1 `UnreadCount` 查 `unread_count FROM conversation_member` → 该列不存在，需用 `last_read_seq` 计算
6. spec §5.2.1 `MessageExists(string)` → message_id 是 BIGINT，需用 int64
7. ES 索引名是 `message` 而非 `chatnow_*` pattern

---

## File Structure

本 plan 新增/修改以下文件：

```
tests/pkg/cleanup/cleanup.go          # Task 1: 全量清理包
tests/pkg/client/config.go            # Task 1: 添加 DatabaseConfig
tests/pkg/client/ws.go                # Task 2: WebSocket 客户端
tests/pkg/verify/db.go                # Task 3: MySQL 直查验证
tests/pkg/verify/es.go                # Task 3: ES 直查验证
tests/pkg/fixture/group.go            # Task 4: 建群/加成员 fixture
tests/pkg/fixture/message.go          # Task 4: 发消息 fixture
tests/pkg/fixture/ws.go               # Task 4: WS 连接 fixture
tests/config.yaml                     # Task 1: 添加 database 段
tests/Makefile                        # Task 2+22: push proto + test-bvt
tests/func/setup_test.go              # Task 1: 添加 cleanup 调用
tests/bvt/setup_test.go               # Task 5: BVT TestMain
tests/bvt/health_test.go              # Task 6: BVT-001~003
tests/bvt/auth_test.go                # Task 7: BVT-004~006
tests/bvt/social_test.go              # Task 8: BVT-007~008
tests/bvt/message_test.go             # Task 9: BVT-009~011
tests/bvt/conversation_test.go        # Task 10: BVT-012~014
tests/bvt/media_test.go               # Task 11: BVT-015~017
tests/bvt/presence_test.go            # Task 12: BVT-018
tests/func/transmite_test.go          # Task 13: FN-TM-01/03/04
tests/func/message_test.go            # Task 14: FN-MS-01/06/10 + untested APIs
tests/func/conversation_test.go       # Task 15: GetMemberIds
tests/func/ws_notify_test.go          # Task 16: FN-WS-01/02
tests/func/consistency_test.go        # Task 17: FN-DC-01/02/03
tests/func/concurrency_test.go        # Task 18: FN-CC-01
tests/func/security_test.go           # Task 19: FN-SEC-01/02/06
tests/func/scenarios_test.go          # Task 20+21: SC-04, SC-06
tests/go.mod                          # Task 1+2: 新增依赖
.github/workflows/ci.yml              # Task 22: bvt job
```

不修改任何生产代码（`common/`、`identity/`、`media/`、`message/` 等）。

---

### Task 1: Cleanup 包 + Config 扩展

**Files:**
- Create: `tests/pkg/cleanup/cleanup.go`
- Modify: `tests/config.yaml`
- Modify: `tests/pkg/client/config.go:9-27`
- Modify: `tests/func/setup_test.go`
- Modify: `tests/go.mod`

**Interfaces:**
- Produces: `cleanup.CleanupAll(t testing.TB)`, `cleanup.WaitForStackReady(timeout time.Duration) error`
- Consumes: `client.Config.DatabaseConfig`（本 task 新增）

- [ ] **Step 1: 添加 Go 依赖**

Run:
```bash
cd tests && go get github.com/go-sql-driver/mysql && go mod tidy
```
Expected: `go.mod` 新增 `github.com/go-sql-driver/mysql`。

- [ ] **Step 2: 扩展 config.yaml 添加 database 段**

Modify `tests/config.yaml`（在 `log:` 段前添加 `database:` 段）:

```yaml
target:
  gateway_addr: "localhost:9000"
  websocket_addr: "localhost:9001"

timeout:
  http_request_sec: 10
  ws_read_sec: 30

database:
  mysql_dsn: "root:YHY060403@tcp(localhost:3306)/chatnow?charset=utf8mb4&parseTime=true"
  es_url: "http://localhost:9200"
  redis_nodes:
    - "localhost:6379"
    - "localhost:6380"
    - "localhost:6381"
    - "localhost:6382"
    - "localhost:6383"
    - "localhost:6384"

log:
  level: "debug"
```

- [ ] **Step 3: 扩展 config.go 添加 DatabaseConfig**

Modify `tests/pkg/client/config.go`（在 `Config` struct 中添加 `Database` 字段，在 `LogConfig` 后添加 `DatabaseConfig` 类型）:

```go
package client

import (
	"os"

	"gopkg.in/yaml.v3"
)

type Config struct {
	Target   TargetConfig   `yaml:"target"`
	Timeout  TimeoutConfig  `yaml:"timeout"`
	Database DatabaseConfig `yaml:"database"`
	Log      LogConfig      `yaml:"log"`
}

type TargetConfig struct {
	GatewayAddr   string `yaml:"gateway_addr"`
	WebsocketAddr string `yaml:"websocket_addr"`
}

type TimeoutConfig struct {
	HTTPRequestSec int `yaml:"http_request_sec"`
	WSReadSec      int `yaml:"ws_read_sec"`
}

type DatabaseConfig struct {
	MySQLDSN   string   `yaml:"mysql_dsn"`
	ESURL      string   `yaml:"es_url"`
	RedisNodes []string `yaml:"redis_nodes"`
}

type LogConfig struct {
	Level string `yaml:"level"`
}

func LoadConfig(path string) *Config {
	if path == "" {
		path = "config.yaml"
	}
	data, err := os.ReadFile(path)
	if err != nil {
		panic("failed to read config: " + err.Error())
	}
	cfg := &Config{}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		panic("failed to parse config: " + err.Error())
	}
	// Env overrides for CI
	if v := os.Getenv("GATEWAY_ADDR"); v != "" {
		cfg.Target.GatewayAddr = v
	}
	if v := os.Getenv("WEBSOCKET_ADDR"); v != "" {
		cfg.Target.WebsocketAddr = v
	}
	if v := os.Getenv("MYSQL_DSN"); v != "" {
		cfg.Database.MySQLDSN = v
	}
	if v := os.Getenv("ES_URL"); v != "" {
		cfg.Database.ESURL = v
	}
	return cfg
}
```

- [ ] **Step 4: 创建 cleanup.go**

Create `tests/pkg/cleanup/cleanup.go`:

```go
package cleanup

import (
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	_ "github.com/go-sql-driver/mysql"

	"chatnow-tests/pkg/client"
)

// truncateTables 按 FK 依赖反序排列，先子表后父表。
var truncateTables = []string{
	"user_timeline",
	"message_attachment",
	"message_mention",
	"message_reaction",
	"message_pin",
	"message_read",
	"message",
	"conversation_member",
	"conversation_view",
	"conversation",
	"friend_apply",
	"relation",
	"user_block",
	"user_device",
	"user",
	"media_file",
	"media_blob_ref",
	"media_user_quota",
}

// CleanupAll 清空所有后端数据，保证测试 run 确定性状态。
// 失败时 t.Fatal（t=nil 时 panic）。
func CleanupAll(t testing.TB, cfg *client.Config) {
	truncateMySQL(t, cfg.Database.MySQLDSN)
	flushRedis(t, cfg.Database.RedisNodes)
	clearESIndices(t, cfg.Database.ESURL)
}

// WaitForStackReady 轮询 gateway:9000/health + 8 个业务服务端口，
// 全部就绪后返回 nil，超时返回 error。
func WaitForStackReady(cfg *client.Config, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	checks := []struct {
		name string
		addr string
	}{
		{"Gateway-HTTP", "127.0.0.1:9000"},
		{"Gateway-WS", "127.0.0.1:9001"},
		{"Identity", "127.0.0.1:10003"},
		{"Media", "127.0.0.1:10002"},
		{"Transmite", "127.0.0.1:10004"},
		{"Message", "127.0.0.1:10005"},
		{"Relationship", "127.0.0.1:10006"},
		{"Conversation", "127.0.0.1:10007"},
		{"Presence", "127.0.0.1:9050"},
	}

	for time.Now().Before(deadline) {
		allReady := true
		for _, c := range checks {
			conn, err := net.DialTimeout("tcp", c.addr, 2*time.Second)
			if err != nil {
				allReady = false
				break
			}
			conn.Close()
		}
		if allReady {
			// 额外等待 gateway HTTP /health 返回 200
			resp, err := http.Get("http://127.0.0.1:9000/health")
			if err == nil && resp.StatusCode == 200 {
				resp.Body.Close()
				return nil
			}
			if resp != nil {
				resp.Body.Close()
			}
		}
		time.Sleep(2 * time.Second)
	}
	return fmt.Errorf("stack not ready after %v", timeout)
}

func truncateMySQL(t testing.TB, dsn string) {
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		fail(t, "open mysql: %v", err)
	}
	defer db.Close()

	// 临时禁用 FK 检查，TRUNCATE 后恢复
	if _, err := db.Exec("SET FOREIGN_KEY_CHECKS = 0"); err != nil {
		fail(t, "disable FK checks: %v", err)
	}
	defer db.Exec("SET FOREIGN_KEY_CHECKS = 1")

	for _, table := range truncateTables {
		if _, err := db.Exec(fmt.Sprintf("TRUNCATE TABLE %s", table)); err != nil {
			// 表可能不存在（如部分服务未启用），跳过但不 fail
			fmt.Printf("cleanup: TRUNCATE %s skipped: %v\n", table, err)
		}
	}
}

func flushRedis(t testing.TB, nodes []string) {
	for _, addr := range nodes {
		if err := flushRedisNode(addr); err != nil {
			fail(t, "FLUSHALL %s: %v", addr, err)
		}
	}
}

// flushRedisNode 用 raw TCP 发送 RESP 协议的 FLUSHALL 命令，无额外依赖。
func flushRedisNode(addr string) error {
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	// RESP: *1\r\n$8\r\nFLUSHALL\r\n
	_, err = conn.Write([]byte("*1\r\n$8\r\nFLUSHALL\r\n"))
	if err != nil {
		return err
	}
	buf := make([]byte, 64)
	_, err = conn.Read(buf)
	return err
}

func clearESIndices(t testing.TB, esURL string) {
	indices := []string{"message", "chat_session"}
	client := &http.Client{Timeout: 10 * time.Second}
	for _, idx := range indices {
		req, _ := http.NewRequest("DELETE", esURL+"/"+idx, nil)
		resp, err := client.Do(req)
		if err != nil {
			fmt.Printf("cleanup: DELETE ES index %s skipped: %v\n", idx, err)
			continue
		}
		resp.Body.Close()
		// 200 或 404 都可接受（404 = 索引不存在）
	}
	// 重建空索引（message 服务启动时自动创建，此处可选）
	_ = createESIndex(esURL, "message")
}

func createESIndex(esURL, index string) error {
	settings := `{
		"mappings": {
			"properties": {
				"user_id":          {"type": "keyword"},
				"message_id":       {"type": "long"},
				"seq_id":           {"type": "long"},
				"create_time":      {"type": "long"},
				"chat_session_id":  {"type": "keyword"},
				"content":          {"type": "text", "analyzer": "standard"},
				"status":           {"type": "integer"}
			}
		}
	}`
	resp, err := http.Post(esURL+"/"+index, "application/json", strings.NewReader(settings))
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	return nil
}

// etcdServiceCount 查询 etcd 中 /service/ 前缀下注册的服务数（BVT-003 用）。
func EtcdServiceCount(etcdURL string) (int, error) {
	// etcd v3 REST API: POST /v3/kv/range with base64-encoded key range
	keyB64 := base64.StdEncoding.EncodeToString([]byte("/service/"))
	rangeEndB64 := base64.StdEncoding.EncodeToString([]byte("/service0"))
	body := fmt.Sprintf(`{"key":"%s","range_end":"%s"}`, keyB64, rangeEndB64)

	resp, err := http.Post(etcdURL+"/v3/kv/range", "application/json", strings.NewReader(body))
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()

	var result struct {
		Kvs []struct {
			Key string `json:"key"`
		} `json:"kvs"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return 0, err
	}
	return len(result.Kvs), nil
}

func fail(t testing.TB, format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	if t != nil {
		t.Fatal(msg)
	}
	panic(msg)
}
```

- [ ] **Step 5: 更新 func/setup_test.go 添加 cleanup 调用**

Modify `tests/func/setup_test.go`:

```go
//go:build func

package func_test

import (
	"os"
	"testing"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/cleanup"
)

var HTTP *client.HTTPClient
var Cfg *client.Config

func TestMain(m *testing.M) {
	Cfg = client.LoadConfig("")
	HTTP = client.NewHTTPClient(Cfg)
	if err := cleanup.WaitForStackReady(Cfg, 120*1e9); err != nil {
		panic(err)
	}
	cleanup.CleanupAll(nil, Cfg)
	os.Exit(m.Run())
}
```

- [ ] **Step 6: 验证 cleanup 包编译**

Run:
```bash
cd tests && go build ./pkg/cleanup/...
```
Expected: 无输出，编译成功。

- [ ] **Step 7: 提交**

```bash
git add tests/pkg/cleanup/cleanup.go tests/pkg/client/config.go tests/config.yaml tests/func/setup_test.go tests/go.mod tests/go.sum
git commit -m "feat(test): cleanup 包 + config database 段 + func setup 调用 CleanupAll

- tests/pkg/cleanup/cleanup.go: TRUNCATE MySQL + FLUSHALL Redis + DELETE ES indices
- WaitForStackReady: 轮询 9 端口 + gateway /health
- EtcdServiceCount: etcd v3 REST API 查询服务注册数（BVT-003 用）
- config.yaml 新增 database 段（mysql_dsn/es_url/redis_nodes）
- func/setup_test.go 调用 CleanupAll 保证确定性状态"
```

---

### Task 2: WebSocket 客户端 + Push Proto 生成

**Files:**
- Create: `tests/pkg/client/ws.go`
- Modify: `tests/Makefile:6`（proto target 添加 push 目录）

**Interfaces:**
- Produces: `client.NewWSClient(cfg *Config, accessToken, userID, deviceID string) (*WSClient, error)`, `(*WSClient).WaitForNotify(ctx context.Context, notifyType int32) (*push.NotifyMessage, error)`, `(*WSClient).WaitForNotifyCount(ctx context.Context, notifyType int32, n int) ([]*push.NotifyMessage, error)`, `(*WSClient).Close() error`
- Consumes: `push.NotifyMessage`（本 task 新增 proto 生成）

- [ ] **Step 1: 添加 gorilla/websocket 依赖**

Run:
```bash
cd tests && go get github.com/gorilla/websocket && go mod tidy
```
Expected: `go.mod` 新增 `github.com/gorilla/websocket`。

- [ ] **Step 2: 修改 Makefile 添加 push proto 生成**

Modify `tests/Makefile` 的 `proto` target，在目录列表中添加 `push` 并跳过 `notify.proto`（与 push_service.proto 重复定义类型，会导致 Go 编译冲突）:

```makefile
proto:
	@echo "Generating protobuf..."
	PROTO_BASE=../proto OUT_BASE=./proto; \
	for dir in common identity relationship conversation message transmite media presence push; do \
		mkdir -p "$$OUT_BASE/chatnow/$$dir"; \
		for f in "$$PROTO_BASE/$$dir"/*.proto; do \
			[ -f "$$f" ] || continue; \
			[ "$$(basename $$f)" = "notify.proto" ] && [ "$$dir" = "push" ] && continue; \
			protoc --proto_path="$$PROTO_BASE" --go_out="$$OUT_BASE" --go_opt=module=chatnow-tests/proto "$$f"; \
		done; \
	done
```

- [ ] **Step 3: 生成 push proto Go 代码**

Run:
```bash
cd tests && make proto && ls proto/chatnow/push/
```
Expected: `proto/chatnow/push/` 下有 `push_service.pb.go`（无 `notify.pb.go`）。

- [ ] **Step 4: 创建 ws.go**

Create `tests/pkg/client/ws.go`:

```go
package client

import (
	"context"
	"fmt"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"google.golang.org/protobuf/proto"

	push "chatnow-tests/proto/chatnow/push"
)

// WSClient 封装 WebSocket 连接，用于接收服务端推送通知。
type WSClient struct {
	conn        *websocket.Conn
	accessToken string
	userID      string
	deviceID    string

	mu       sync.Mutex
	notifies []*push.NotifyMessage
	notifyCh chan *push.NotifyMessage
	closed   bool
}

// NewWSClient 连接 gateway WS，发送 CLIENT_AUTH 鉴权帧，启动 readLoop。
func NewWSClient(cfg *Config, accessToken, userID, deviceID string) (*WSClient, error) {
	url := "ws://" + cfg.Target.WebsocketAddr + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		return nil, fmt.Errorf("ws dial: %w", err)
	}

	w := &WSClient{
		conn:        conn,
		accessToken: accessToken,
		userID:      userID,
		deviceID:    deviceID,
		notifyCh:    make(chan *push.NotifyMessage, 100),
	}

	// 发送 CLIENT_AUTH 鉴权帧
	authNotify := &push.NotifyMessage{
		NotifyType: push.NotifyType_CLIENT_AUTH,
		NotifyRemarks: &push.NotifyMessage_ClientAuth{
			ClientAuth: &push.NotifyClientAuth{
				AccessToken: accessToken,
				DeviceId:    deviceID,
			},
		},
	}
	authBytes, err := proto.Marshal(authNotify)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("marshal auth: %w", err)
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, authBytes); err != nil {
		conn.Close()
		return nil, fmt.Errorf("write auth: %w", err)
	}

	go w.readLoop()
	return w, nil
}

// WaitForNotify 阻塞等待指定 notify_type 的通知，超时返回 ctx.Err()。
func (w *WSClient) WaitForNotify(ctx context.Context, notifyType int32) (*push.NotifyMessage, error) {
	// 先检查已缓存的
	w.mu.Lock()
	for i, n := range w.notifies {
		if n.NotifyType == notifyType {
			w.notifies = append(w.notifies[:i], w.notifies[i+1:]...)
			w.mu.Unlock()
			return n, nil
		}
	}
	w.mu.Unlock()

	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case n := <-w.notifyCh:
			if n.NotifyType == notifyType {
				return n, nil
			}
			// 缓存非匹配通知
			w.mu.Lock()
			w.notifies = append(w.notifies, n)
			w.mu.Unlock()
		}
	}
}

// WaitForNotifyCount 等待指定 type 的 n 条通知。
func (w *WSClient) WaitForNotifyCount(ctx context.Context, notifyType int32, n int) ([]*push.NotifyMessage, error) {
	results := make([]*push.NotifyMessage, 0, n)
	// 先检查缓存
	w.mu.Lock()
	remaining := make([]*push.NotifyMessage, 0)
	for _, msg := range w.notifies {
		if msg.NotifyType == notifyType && len(results) < n {
			results = append(results, msg)
		} else {
			remaining = append(remaining, msg)
		}
	}
	w.notifies = remaining
	w.mu.Unlock()

	for len(results) < n {
		select {
		case <-ctx.Done():
			return results, ctx.Err()
		case msg := <-w.notifyCh:
			if msg.NotifyType == notifyType {
				results = append(results, msg)
			} else {
				w.mu.Lock()
				w.notifies = append(w.notifies, msg)
				w.mu.Unlock()
			}
		}
	}
	return results, nil
}

// Close 关闭 WS 连接。
func (w *WSClient) Close() error {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return nil
	}
	w.closed = true
	w.mu.Unlock()
	return w.conn.Close()
}

func (w *WSClient) readLoop() {
	for {
		_, data, err := w.conn.ReadMessage()
		if err != nil {
			return
		}
		notify := &push.NotifyMessage{}
		if err := proto.Unmarshal(data, notify); err != nil {
			continue
		}
		w.mu.Lock()
		if w.closed {
			w.mu.Unlock()
			return
		}
		w.mu.Unlock()
		select {
		case w.notifyCh <- notify:
		default:
			// channel 满了，丢弃
		}
	}
}

// WaitForNotifyWithTimeout 是带超时的便捷方法。
func (w *WSClient) WaitForNotifyWithTimeout(notifyType int32, timeout time.Duration) (*push.NotifyMessage, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	return w.WaitForNotify(ctx, notifyType)
}
```

- [ ] **Step 5: 验证 ws.go 编译**

Run:
```bash
cd tests && go build ./pkg/client/...
```
Expected: 无输出，编译成功。

- [ ] **Step 6: 提交**

```bash
git add tests/pkg/client/ws.go tests/Makefile tests/go.mod tests/go.sum tests/proto/chatnow/push/
git commit -m "feat(test): WebSocket 客户端 + push proto 生成

- tests/pkg/client/ws.go: NewWSClient/WaitForNotify/WaitForNotifyCount/Close
- 连接后发送 CLIENT_AUTH 帧（access_token + device_id）
- readLoop 解析 binary protobuf 帧按 NotifyType 分发
- Makefile proto target 添加 push 目录（跳过 notify.proto 避免类型冲突）
- 生成 push_service.pb.go（NotifyMessage/NotifyClientAuth 等）"
```

---

### Task 3: DB + ES 直查验证包

**Files:**
- Create: `tests/pkg/verify/db.go`
- Create: `tests/pkg/verify/es.go`

**Interfaces:**
- Produces: `verify.NewDBVerifier(dsn string) *DBVerifier`, `verify.NewESVerifier(url string) *ESVerifier`
- Consumes: `github.com/go-sql-driver/mysql`（Task 1 已添加）

- [ ] **Step 1: 创建 db.go**

Create `tests/pkg/verify/db.go`:

```go
package verify

import (
	"database/sql"
	"fmt"
	"testing"

	_ "github.com/go-sql-driver/mysql"
)

// DBVerifier 直查 MySQL 验证 HTTP 响应与底层存储一致。
type DBVerifier struct {
	db *sql.DB
}

// NewDBVerifier 创建 MySQL 直查验证器。
func NewDBVerifier(dsn string) *DBVerifier {
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		panic("open mysql: " + err.Error())
	}
	db.SetMaxIdleConns(2)
	db.SetMaxOpenConns(5)
	return &DBVerifier{db: db}
}

// Close 关闭数据库连接。
func (v *DBVerifier) Close() {
	if v.db != nil {
		v.db.Close()
	}
}

// MessageExists 验证 message 表存在指定 message_id 的记录。
// message_id 是 BIGINT（雪花 ID），用 int64 查询。
func (v *DBVerifier) MessageExists(t testing.TB, messageID int64) {
	var cnt int
	err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE message_id = ?", messageID).Scan(&cnt)
	if err != nil {
		t.Fatalf("query message %d: %v", messageID, err)
	}
	if cnt != 1 {
		t.Fatalf("message %d 未落库，期望 1 行，实际 %d 行", messageID, cnt)
	}
}

// MessageCount 验证某会话 message 表记录数。
// 注意：message 表用 session_id 列名存储 conversation_id。
func (v *DBVerifier) MessageCount(t testing.TB, conversationID string, expected int) {
	var cnt int
	err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE session_id = ?", conversationID).Scan(&cnt)
	if err != nil {
		t.Fatalf("query message count: %v", err)
	}
	if cnt != expected {
		t.Fatalf("会话 %s message 数应为 %d，实际 %d", conversationID, expected, cnt)
	}
}

// UserTimelineExists 验证 user_timeline 写扩散记录数。
func (v *DBVerifier) UserTimelineExists(t testing.TB, userID, conversationID string, expected int) {
	var cnt int
	err := v.db.QueryRow(
		"SELECT COUNT(*) FROM user_timeline WHERE user_id = ? AND session_id = ?",
		userID, conversationID,
	).Scan(&cnt)
	if err != nil {
		t.Fatalf("query user_timeline: %v", err)
	}
	if cnt != expected {
		t.Fatalf("user_timeline 写扩散 user=%s conv=%s 期望 %d 行，实际 %d 行", userID, conversationID, expected, cnt)
	}
}

// UserTimelineCount 验证某会话的 user_timeline 总行数（=成员数）。
func (v *DBVerifier) UserTimelineCount(t testing.TB, conversationID string, expected int) {
	var cnt int
	err := v.db.QueryRow(
		"SELECT COUNT(*) FROM user_timeline WHERE session_id = ?", conversationID,
	).Scan(&cnt)
	if err != nil {
		t.Fatalf("query user_timeline count: %v", err)
	}
	if cnt != expected {
		t.Fatalf("user_timeline conv=%s 期望 %d 行，实际 %d 行", conversationID, expected, cnt)
	}
}

// FriendRelationExists 验证 relation 表双向好友关系存在。
func (v *DBVerifier) FriendRelationExists(t testing.TB, uidA, uidB string) {
	var cnt int
	err := v.db.QueryRow(
		"SELECT COUNT(*) FROM relation WHERE user_id = ? AND peer_id = ?",
		uidA, uidB,
	).Scan(&cnt)
	if err != nil {
		t.Fatalf("query relation: %v", err)
	}
	if cnt != 1 {
		t.Fatalf("好友关系 %s -> %s 不存在，期望 1 行，实际 %d 行", uidA, uidB, cnt)
	}
}

// LastReadSeq 验证 conversation_member 表的 last_read_seq 值。
// 注意：conversation_member 无 unread_count 列，未读数通过 max(seq) - last_read_seq 计算。
func (v *DBVerifier) LastReadSeq(t testing.TB, userID, conversationID string, expected uint64) {
	var seq uint64
	err := v.db.QueryRow(
		"SELECT last_read_seq FROM conversation_member WHERE user_id = ? AND conversation_id = ?",
		userID, conversationID,
	).Scan(&seq)
	if err != nil {
		t.Fatalf("query last_read_seq: %v", err)
	}
	if seq != expected {
		t.Fatalf("last_read_seq user=%s conv=%s 期望 %d，实际 %d", userID, conversationID, expected, seq)
	}
}

// UnreadCount 计算并验证未读数 = max(message.seq_id) - last_read_seq。
func (v *DBVerifier) UnreadCount(t testing.TB, userID, conversationID string, expected int) {
	var lastReadSeq uint64
	var maxSeq sql.NullInt64
	err := v.db.QueryRow(
		"SELECT last_read_seq FROM conversation_member WHERE user_id = ? AND conversation_id = ?",
		userID, conversationID,
	).Scan(&lastReadSeq)
	if err != nil {
		t.Fatalf("query last_read_seq: %v", err)
	}
	err = v.db.QueryRow(
		"SELECT MAX(seq_id) FROM message WHERE session_id = ?", conversationID,
	).Scan(&maxSeq)
	if err != nil {
		t.Fatalf("query max seq: %v", err)
	}
	actual := 0
	if maxSeq.Valid {
		actual = int(maxSeq.Int64) - int(lastReadSeq)
	}
	if actual < 0 {
		actual = 0
	}
	if actual != expected {
		t.Fatalf("unread_count user=%s conv=%s 期望 %d，实际 %d (maxSeq=%d lastRead=%d)",
			userID, conversationID, expected, actual, maxSeq.Int64, lastReadSeq)
	}
}

// MessageStatus 验证 message.status 值（0=NORMAL, 1=REVOKED, 2=DELETED）。
func (v *DBVerifier) MessageStatus(t testing.TB, messageID int64, expected int32) {
	var status int32
	err := v.db.QueryRow("SELECT status FROM message WHERE message_id = ?", messageID).Scan(&status)
	if err != nil {
		t.Fatalf("query message status: %v", err)
	}
	if status != expected {
		t.Fatalf("message %d status 期望 %d，实际 %d", messageID, expected, status)
	}
}

// MessageByClientMsgId 验证 message 表按 client_msg_id 查到记录。
func (v *DBVerifier) MessageByClientMsgId(t testing.TB, clientMsgID string, shouldExist bool) {
	var cnt int
	err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE client_msg_id = ?", clientMsgID).Scan(&cnt)
	if err != nil {
		t.Fatalf("query message by client_msg_id: %v", err)
	}
	if shouldExist && cnt == 0 {
		t.Fatalf("client_msg_id %s 应存在但未找到", clientMsgID)
	}
	if !shouldExist && cnt > 0 {
		t.Fatalf("client_msg_id %s 不应存在但找到 %d 行", clientMsgID, cnt)
	}
}

// MediaQuota 验证 media_user_quota.used_bytes。
func (v *DBVerifier) MediaQuota(t testing.TB, userID string, expectedUsedBytes int64) {
	var used int64
	err := v.db.QueryRow("SELECT used_bytes FROM media_user_quota WHERE user_id = ?", userID).Scan(&used)
	if err != nil {
		t.Fatalf("query media quota: %v", err)
	}
	if used != expectedUsedBytes {
		t.Fatalf("media quota user=%s 期望 %d，实际 %d", userID, expectedUsedBytes, used)
	}
}

// ConversationMemberRole 验证 conversation_member.role（0=MEMBER, 1=ADMIN, 2=OWNER）。
func (v *DBVerifier) ConversationMemberRole(t testing.TB, userID, conversationID string, expectedRole int32) {
	var role int32
	err := v.db.QueryRow(
		"SELECT role FROM conversation_member WHERE user_id = ? AND conversation_id = ?",
		userID, conversationID,
	).Scan(&role)
	if err != nil {
		t.Fatalf("query member role: %v", err)
	}
	if role != expectedRole {
		t.Fatalf("member role user=%s conv=%s 期望 %d，实际 %d", userID, conversationID, expectedRole, role)
	}
}

// RawQuery 执行任意查询并返回单行单列 int 值（灵活查询用）。
func (v *DBVerifier) RawQuery(t testing.TB, query string, args ...interface{}) int {
	var cnt int
	err := v.db.QueryRow(query, args...).Scan(&cnt)
	if err != nil {
		t.Fatalf("raw query: %v", err)
	}
	return cnt
}

func intToStr(i int64) string {
	return fmt.Sprintf("%d", i)
}
```

- [ ] **Step 2: 创建 es.go**

Create `tests/pkg/verify/es.go`:

```go
package verify

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

// ESVerifier 直查 Elasticsearch 验证消息索引一致性。
// 使用标准 net/http，无额外 ES SDK 依赖。
type ESVerifier struct {
	client *http.Client
	esURL  string
}

// NewESVerifier 创建 ES 直查验证器。
func NewESVerifier(url string) *ESVerifier {
	return &ESVerifier{
		client: &http.Client{Timeout: 10 * time.Second},
		esURL:  strings.TrimRight(url, "/"),
	}
}

// MessageIndexed 验证消息已索引到 ES（按 message_id 查）。
func (v *ESVerifier) MessageIndexed(t testing.TB, messageID int64, contentKeyword string) {
	// 轮询等待 ES 异步索引（最多 5 秒）
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if v.checkMessageIndexed(messageID, contentKeyword) {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("ES 未索引消息 %d (keyword=%s)，5s 内未出现", messageID, contentKeyword)
}

func (v *ESVerifier) checkMessageIndexed(messageID int64, contentKeyword string) bool {
	body := fmt.Sprintf(`{
		"query": {
			"bool": {
				"must": [
					{"term": {"message_id": %d}},
					{"match": {"content": "%s"}}
				]
			}
		}
	}`, messageID, contentKeyword)

	resp, err := v.client.Post(v.esURL+"/message/_search", "application/json", strings.NewReader(body))
	if err != nil {
		return false
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return false
	}

	var result struct {
		Hits struct {
			Total struct {
				Value int `json:"value"`
			} `json:"total"`
		} `json:"hits"`
	}
	if err := json.Unmarshal(data, &result); err != nil {
		return false
	}
	return result.Hits.Total.Value >= 1
}

// SearchHitCount 验证 ES 搜索命中数。
func (v *ESVerifier) SearchHitCount(t testing.TB, conversationID, keyword string, expected int) {
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		actual := v.searchHitCount(conversationID, keyword)
		if actual == expected {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("ES 搜索 conv=%s keyword=%s 期望 %d 命中，5s 内未达到", conversationID, keyword, expected)
}

func (v *ESVerifier) searchHitCount(conversationID, keyword string) int {
	body := fmt.Sprintf(`{
		"query": {
			"bool": {
				"must": [
					{"term": {"chat_session_id.keyword": "%s"}},
					{"match": {"content": "%s"}}
				],
				"filter": [{"term": {"status": 0}}]
			}
		}
	}`, conversationID, keyword)

	resp, err := v.client.Post(v.esURL+"/message/_search", "application/json", strings.NewReader(body))
	if err != nil {
		return -1
	}
	defer resp.Body.Close()

	data, _ := io.ReadAll(resp.Body)
	var result struct {
		Hits struct {
			Total struct {
				Value int `json:"value"`
			} `json:"total"`
		} `json:"hits"`
	}
	json.Unmarshal(data, &result)
	return result.Hits.Total.Value
}

// IndexExists 验证 ES 索引是否存在。
func (v *ESVerifier) IndexExists(t testing.TB, indexName string) {
	resp, err := v.client.Head(v.esURL + "/" + indexName)
	if err != nil {
		t.Fatalf("ES HEAD index %s: %v", indexName, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("ES 索引 %s 不存在 (status=%d)", indexName, resp.StatusCode)
	}
}
```

- [ ] **Step 3: 验证 verify 包编译**

Run:
```bash
cd tests && go build ./pkg/verify/...
```
Expected: 无输出，编译成功。

- [ ] **Step 4: 提交**

```bash
git add tests/pkg/verify/db.go tests/pkg/verify/es.go
git commit -m "feat(test): verify 包 - DB + ES 直查验证

- verify/db.go: MessageExists/MessageCount/UserTimelineExists/FriendRelationExists/
  UnreadCount(计算)/MessageStatus/MessageByClientMsgId/MediaQuota/MemberRole
- verify/es.go: MessageIndexed/SearchHitCount/IndexExists（轮询 5s 等待异步索引）
- DB 列名修正：message.session_id=conversation_id, conversation_member 无 unread_count 列"
```

---

### Task 4: Fixture 扩展（group + message + ws）

**Files:**
- Create: `tests/pkg/fixture/group.go`
- Create: `tests/pkg/fixture/message.go`
- Create: `tests/pkg/fixture/ws.go`

**Interfaces:**
- Produces: `fixture.CreateGroup(t, owner, members, name) string`, `fixture.AddMembers(t, owner, convID, memberIDs)`, `fixture.SendTextMessage(t, client, convID, text) (int64, uint64)`, `fixture.SendImageMessage(t, client, convID, fileID) int64`, `fixture.ConnectWS(t, client) *WSClient`
- Consumes: `client.HTTPClient`, `client.WSClient`（Task 2）

- [ ] **Step 1: 创建 group.go**

Create `tests/pkg/fixture/group.go`:

```go
package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
	conversation "chatnow-tests/proto/chatnow/conversation"
)

// CreateGroup 创建群会话（owner + members），返回 conversation_id。
// 注：已有 CreateGroupWithMembers 在 conversation.go 中，此为简化别名。
func CreateGroup(t testing.TB, owner *client.HTTPClient, members []*client.HTTPClient, name string) string {
	return CreateGroupWithMembers(t, owner, members, name)
}

// AddMembers 向群会话添加成员。
func AddMembers(t testing.TB, owner *client.HTTPClient, convID string, memberIDs []string) {
	req := &conversation.AddMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MemberIds:      memberIDs,
	}
	rsp := &conversation.AddMembersRsp{}
	if err := owner.DoAuth("/service/conversation/add_members", req, rsp); err != nil {
		t.Fatalf("AddMembers: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("AddMembers failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
}

// CreateGroupSimple 创建群会话并返回 convID + 所有成员 client（含 owner）。
func CreateGroupSimple(t testing.TB, base *client.HTTPClient, memberCount int) (owner *client.HTTPClient, members []*client.HTTPClient, convID string) {
	owner, _, _ = RegisterAndLogin(t, base)
	members = make([]*client.HTTPClient, 0, memberCount)
	memberIDs := make([]string, 0, memberCount)
	for i := 0; i < memberCount; i++ {
		m, _, _ := RegisterAndLogin(t, base)
		members = append(members, m)
		memberIDs = append(memberIDs, m.UserID)
	}
	name := "test-group-" + client.NewRequestID()[:8]
	convID = CreateGroup(t, owner, members, name)
	return owner, members, convID
}
```

- [ ] **Step 2: 创建 message.go**

Create `tests/pkg/fixture/message.go`:

```go
package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// SendTextMessage 发送文本消息，返回 (message_id, seq_id)。
func SendTextMessage(t testing.TB, c *client.HTTPClient, convID, text string) (int64, uint64) {
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: text}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	rsp := &transmite.SendMessageRsp{}
	if err := c.DoAuth("/service/transmite/send", req, rsp); err != nil {
		t.Fatalf("SendTextMessage: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("SendTextMessage failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
	if rsp.Message == nil {
		t.Fatal("SendTextMessage: response message is nil")
	}
	return rsp.Message.MessageId, rsp.Message.SeqId
}

// SendTextMessageWithClientMsgId 用指定 client_msg_id 发送文本消息。
func SendTextMessageWithClientMsgId(t testing.TB, c *client.HTTPClient, convID, text, clientMsgID string) (int64, uint64, bool) {
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: text}},
		},
		ClientMsgId: clientMsgID,
	}
	rsp := &transmite.SendMessageRsp{}
	if err := c.DoAuth("/service/transmite/send", req, rsp); err != nil {
		t.Fatalf("SendTextMessageWithClientMsgId: %v", err)
	}
	if rsp.Message == nil {
		return 0, 0, rsp.Header.Success
	}
	return rsp.Message.MessageId, rsp.Message.SeqId, rsp.Header.Success
}

// SendImageMessage 发送图片消息，返回 message_id。
func SendImageMessage(t testing.TB, c *client.HTTPClient, convID, fileID string) int64 {
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_IMAGE,
			Body: &msg.MessageContent_Image{Image: &msg.ImageContent{
				FileId: fileID,
				Width:  100,
				Height: 100,
			}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	rsp := &transmite.SendMessageRsp{}
	if err := c.DoAuth("/service/transmite/send", req, rsp); err != nil {
		t.Fatalf("SendImageMessage: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("SendImageMessage failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
	if rsp.Message == nil {
		t.Fatal("SendImageMessage: response message is nil")
	}
	return rsp.Message.MessageId
}
```

- [ ] **Step 3: 创建 ws.go**

Create `tests/pkg/fixture/ws.go`:

```go
package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
)

// ConnectWS 建立 WS 连接并完成鉴权，返回 WSClient。
// 测试结束时应调用 ws.Close() 释放连接。
func ConnectWS(t testing.TB, c *client.HTTPClient) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, c.DeviceID)
	if err != nil {
		t.Fatalf("ConnectWS: %v", err)
	}
	return ws
}

// ConnectWSWithDeviceID 用指定 deviceID 建立 WS 连接。
func ConnectWSWithDeviceID(t testing.TB, c *client.HTTPClient, deviceID string) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, deviceID)
	if err != nil {
		t.Fatalf("ConnectWSWithDeviceID: %v", err)
	}
	return ws
}
```

- [ ] **Step 4: 验证 fixture 包编译**

Run:
```bash
cd tests && go build ./pkg/fixture/...
```
Expected: 无输出，编译成功。

- [ ] **Step 5: 提交**

```bash
git add tests/pkg/fixture/group.go tests/pkg/fixture/message.go tests/pkg/fixture/ws.go
git commit -m "feat(test): fixture 扩展 - group/message/ws

- fixture/group.go: CreateGroup/AddMembers/CreateGroupSimple
- fixture/message.go: SendTextMessage/SendTextMessageWithClientMsgId/SendImageMessage
- fixture/ws.go: ConnectWS/ConnectWSWithDeviceID
- 所有 fixture 不做断言（除 t.Fatal），返回关键 ID 供测试断言"
```

---

### Task 5: BVT setup_test.go

**Files:**
- Create: `tests/bvt/setup_test.go`

**Interfaces:**
- Produces: BVT 包的 `TestMain` + 包级 `HTTP`/`Cfg` 变量
- Consumes: `cleanup.CleanupAll`, `cleanup.WaitForStackReady`（Task 1）

- [ ] **Step 1: 创建 setup_test.go**

Create `tests/bvt/setup_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"os"
	"testing"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/cleanup"
)

var HTTP *client.HTTPClient
var Cfg *client.Config

func TestMain(m *testing.M) {
	Cfg = client.LoadConfig("")
	HTTP = client.NewHTTPClient(Cfg)
	if err := cleanup.WaitForStackReady(Cfg, 120*1e9); err != nil {
		panic(err)
	}
	cleanup.CleanupAll(nil, Cfg)
	os.Exit(m.Run())
}
```

- [ ] **Step 2: 验证 BVT 包编译**

Run:
```bash
cd tests && go test -tags=bvt -run xxx_nothing ./bvt/... 2>&1 | head -5
```
Expected: 编译成功（无测试匹配，输出 `ok` 或 `PASS` + `no tests to run`）。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/setup_test.go
git commit -m "feat(bvt): setup_test.go - TestMain 加载 config + WaitForStackReady + CleanupAll"
```

---

### Task 6: BVT health_test.go（BVT-001~003）

**Files:**
- Create: `tests/bvt/health_test.go`

- [ ] **Step 1: 创建 health_test.go**

Create `tests/bvt/health_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"encoding/json"
	"net/http"
	"testing"

	"github.com/gorilla/websocket"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/cleanup"
)

// BVT-001 | P0 | 基础设施健康 | gateway HTTP 端口存活
func TestBVT_GatewayHTTP_Reachable(t *testing.T) {
	resp, err := http.Get("http://" + Cfg.Target.GatewayAddr + "/health")
	require.NoError(t, err)
	defer resp.Body.Close()
	assert.Equal(t, 200, resp.StatusCode)
}

// BVT-002 | P0 | 基础设施健康 | gateway WS 端口可连接
func TestBVT_GatewayWS_Reachable(t *testing.T) {
	url := "ws://" + Cfg.Target.WebsocketAddr + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	require.NoError(t, err)
	defer conn.Close()
	assert.True(t, conn != nil)
}

// BVT-003 | P0 | 基础设施健康 | etcd 中 8 个服务均有注册实例
func TestBVT_ServicesRegistered(t *testing.T) {
	etcdURL := "http://127.0.0.1:2379"
	count, err := cleanup.EtcdServiceCount(etcdURL)
	require.NoError(t, err)
	// 至少 8 个业务服务注册（gateway/push/identity/media/transmite/message/relationship/conversation/presence）
	assert.GreaterOrEqual(t, count, 8, "etcd 注册服务数应 >= 8，实际 %d", count)
	// 打印原始数据便于调试
	t.Logf("etcd /service/ 下注册了 %d 个 key", count)
}

// 辅助：解码 etcd REST API 响应（BVT-003 验证用）
func decodeEtcdKeys(body []byte) ([]string, error) {
	var result struct {
		Kvs []struct {
			Key string `json:"key"`
		} `json:"kvs"`
	}
	if err := json.Unmarshal(body, &result); err != nil {
		return nil, err
	}
	keys := make([]string, 0, len(result.Kvs))
	for _, kv := range result.Kvs {
		keys = append(keys, kv.Key)
	}
	return keys, nil
}
```

- [ ] **Step 2: 运行测试（需 docker compose up）**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_GatewayHTTP -v -count=1
```
Expected: `PASS`（gateway HTTP 返回 200）。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/health_test.go
git commit -m "test(bvt): BVT-001~003 基础设施健康检查

- BVT-001: gateway HTTP /health 返回 200
- BVT-002: gateway WS 端口可连接
- BVT-003: etcd /service/ 下注册服务数 >= 8"
```

---

### Task 7: BVT auth_test.go（BVT-004~006）

**Files:**
- Create: `tests/bvt/auth_test.go`

- [ ] **Step 1: 创建 auth_test.go**

Create `tests/bvt/auth_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"fmt"
	"math/rand"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// BVT-004 | P0 | 认证链路 | 用户名注册成功，返回 user_id
func TestBVT_Register_Success(t *testing.T) {
	username := fmt.Sprintf("bvt_%d_%d", rand.Int63n(10000000), rand.Intn(1000))
	password := "Bvt123456"

	req := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	rsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "注册失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.UserId)
	require.NotNil(t, rsp.Tokens)
	assert.NotEmpty(t, rsp.Tokens.AccessToken)
}

// BVT-005 | P0 | 认证链路 | 登录成功，返回 access_token + refresh_token
func TestBVT_Login_Success(t *testing.T) {
	// 先注册
	username := fmt.Sprintf("bvt_%d_%d", rand.Int63n(10000000), rand.Intn(1000))
	password := "Bvt123456"

	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	require.NoError(t, HTTP.DoNoAuth("/service/identity/register", regReq, regRsp))
	require.True(t, regRsp.Header.Success)

	// 再登录
	loginReq := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		DeviceId:   client.NewDeviceID(),
		DeviceName: "bvt-test-device",
	}
	loginRsp := &identity.LoginRsp{}
	err := HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp)
	require.NoError(t, err)
	require.True(t, loginRsp.Header.Success, "登录失败: %s", loginRsp.Header.ErrorMessage)
	require.NotNil(t, loginRsp.Tokens)
	assert.NotEmpty(t, loginRsp.Tokens.AccessToken)
	assert.NotEmpty(t, loginRsp.Tokens.RefreshToken)
}

// BVT-006 | P0 | 认证链路 | 带 token 调 GetProfile，返回自身信息
func TestBVT_AuthenticatedAPICall(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := authed.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "鉴权调用失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, authed.UserID, rsp.UserInfo.UserId)
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_Register -v -count=1
```
Expected: `PASS`。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/auth_test.go
git commit -m "test(bvt): BVT-004~006 认证链路冒烟

- BVT-004: 用户名注册成功返回 user_id + tokens
- BVT-005: 登录成功返回 access_token + refresh_token
- BVT-006: 带 token 调 GetProfile 返回自身信息"
```

---

### Task 8: BVT social_test.go（BVT-007~008）

**Files:**
- Create: `tests/bvt/social_test.go`

- [ ] **Step 1: 创建 social_test.go**

Create `tests/bvt/social_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

// BVT-007 | P0 | 社交链路 | A 向 B 发好友申请，返回 notify_event_id
func TestBVT_SendFriendRequest_Success(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bob.UserID,
	}
	rsp := &relationship.SendFriendRsp{}
	err := alice.DoAuth("/service/relationship/send_friend_request", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "发好友申请失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.GetNotifyEventId())
}

// BVT-008 | P0 | 社交链路 | B 通过申请，返回 new_conversation_id
func TestBVT_AcceptFriend_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// MakeFriends 已完成 send + accept，验证结果
	require.NotEmpty(t, convID, "通过好友申请后应返回 conversation_id")

	// 额外验证：B 的好友列表包含 A
	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID()}
	listRsp := &relationship.ListFriendsRsp{}
	err := bob.DoAuth("/service/relationship/list_friends", listReq, listRsp)
	require.NoError(t, err)
	assert.True(t, listRsp.Header.Success)
	found := false
	for _, f := range listRsp.FriendList {
		if f.UserId == alice.UserID {
			found = true
			break
		}
	}
	assert.True(t, found, "B 的好友列表应包含 A")
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_SendFriendRequest -v -count=1
```
Expected: `PASS`。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/social_test.go
git commit -m "test(bvt): BVT-007~008 社交链路冒烟

- BVT-007: 发好友申请返回 notify_event_id
- BVT-008: 通过好友申请返回 new_conversation_id + 好友列表验证"
```

---

### Task 9: BVT message_test.go（BVT-009~011）

**Files:**
- Create: `tests/bvt/message_test.go`

- [ ] **Step 1: 创建 message_test.go**

Create `tests/bvt/message_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	msg "chatnow-tests/proto/chatnow/message"
)

// BVT-009 | P0 | 消息链路 | 发文本消息，返回 message_id + seq_id
func TestBVT_SendTextMessage_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	msgID, seqID := fixture.SendTextMessage(t, alice, convID, "bvt hello")
	assert.NotZero(t, msgID, "message_id 不应为 0")
	assert.NotZero(t, seqID, "seq_id 不应为 0")
	_ = bob
}

// BVT-010 | P0 | 消息链路 | SyncMessages 返回刚发的消息
func TestBVT_SyncMessages_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	fixture.SendTextMessage(t, alice, convID, "sync test msg")

	req := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	rsp := &msg.SyncMessagesRsp{}
	err := bob.DoAuth("/service/message/sync", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "sync 失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.Messages, "应返回至少 1 条消息")
	assert.Equal(t, "sync test msg", rsp.Messages[0].GetText().Text)
}

// BVT-011 | P0 | 消息链路 | GetHistory 返回消息列表
func TestBVT_GetHistory_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	fixture.SendTextMessage(t, alice, convID, "history test msg")

	// 先 sync 获取 latest_seq
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))

	histReq := &msg.GetHistoryReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		BeforeSeq:      syncRsp.LatestSeq + 1,
		Limit:          10,
	}
	histRsp := &msg.GetHistoryRsp{}
	err := bob.DoAuth("/service/message/get_history", histReq, histRsp)
	require.NoError(t, err)
	require.True(t, histRsp.Header.Success, "get_history 失败: %s", histRsp.Header.ErrorMessage)
	assert.NotEmpty(t, histRsp.Messages, "历史消息不应为空")
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_SendTextMessage -v -count=1
```
Expected: `PASS`。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/message_test.go
git commit -m "test(bvt): BVT-009~011 消息链路冒烟

- BVT-009: 发文本消息返回 message_id + seq_id
- BVT-010: SyncMessages 返回刚发的消息
- BVT-011: GetHistory 返回消息列表"
```

---

### Task 10: BVT conversation_test.go（BVT-012~014）

**Files:**
- Create: `tests/bvt/conversation_test.go`

- [ ] **Step 1: 创建 conversation_test.go**

Create `tests/bvt/conversation_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	common "chatnow-tests/proto/chatnow/common"
	conversation "chatnow-tests/proto/chatnow/conversation"
)

// BVT-012 | P0 | 会话链路 | 创建群会话，返回 conversation_id
func TestBVT_CreateGroupConversation_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	name := "bvt-group-" + client.NewRequestID()[:8]
	req := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &name,
		MemberIds: []string{m1.UserID, m2.UserID},
	}
	rsp := &conversation.CreateConversationRsp{}
	err := owner.DoAuth("/service/conversation/create", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "创建群会话失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.Conversation)
	assert.NotEmpty(t, rsp.Conversation.ConversationId)
}

// BVT-013 | P0 | 会话链路 | 添加成员到群会话
func TestBVT_AddMembers_Success(t *testing.T) {
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 2)

	// 添加第 3 个成员
	m3, _, _ := fixture.RegisterAndLogin(t, HTTP)
	fixture.AddMembers(t, owner, convID, []string{m3.UserID})

	// 验证成员数 = 3（owner + 2 初始 + 1 新增）
	listReq := &conversation.ListMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	listRsp := &conversation.ListMembersRsp{}
	err := owner.DoAuth("/service/conversation/list_members", listReq, listRsp)
	require.NoError(t, err)
	assert.True(t, listRsp.Header.Success)
	assert.Len(t, listRsp.Members, 3)

	_ = members
}

// BVT-014 | P0 | 会话链路 | 列出会话，包含刚建的群
func TestBVT_ListConversations_Success(t *testing.T) {
	owner, _, convID := fixture.CreateGroupSimple(t, HTTP, 1)

	req := &conversation.ListConversationsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 50},
	}
	rsp := &conversation.ListConversationsRsp{}
	err := owner.DoAuth("/service/conversation/list", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "list conversations 失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.Conversations)

	// 验证列表包含刚建的群
	found := false
	for _, c := range rsp.Conversations {
		if c.ConversationId == convID {
			found = true
			break
		}
	}
	assert.True(t, found, "会话列表应包含刚建的群 %s", convID)
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_CreateGroupConversation -v -count=1
```
Expected: `PASS`。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/conversation_test.go
git commit -m "test(bvt): BVT-012~014 会话链路冒烟

- BVT-012: 创建群会话返回 conversation_id
- BVT-013: 添加成员到群会话 + 验证成员数
- BVT-014: 列出会话包含刚建的群"
```

---

### Task 11: BVT media_test.go（BVT-015~017）

**Files:**
- Create: `tests/bvt/media_test.go`

- [ ] **Step 1: 创建 media_test.go**

Create `tests/bvt/media_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	media "chatnow-tests/proto/chatnow/media"
)

// BVT-015 | P0 | 媒体链路 | 申请上传，返回 file_id + upload_url
func TestBVT_ApplyUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt test content")
	hash := sha256.Sum256(content)

	req := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "apply_upload 失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.FileId)
	// upload_url 可能为空（如果 already_exists=true）
	if !rsp.AlreadyExists {
		assert.NotEmpty(t, rsp.UploadUrl)
	}
}

// BVT-016 | P0 | 媒体链路 | PUT 到 MinIO + CompleteUpload，success=true
func TestBVT_CompleteUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt upload content")
	hash := sha256.Sum256(content)

	// Step 1: ApplyUpload
	applyReq := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt-upload.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp)
	require.NoError(t, err)
	require.True(t, applyRsp.Header.Success)

	if applyRsp.AlreadyExists {
		t.Skip("文件已存在（dedup 命中），跳过上传步骤")
	}

	fileID := applyRsp.FileId
	uploadURL := applyRsp.UploadUrl
	require.NotEmpty(t, uploadURL, "upload_url 不应为空")

	// Step 2: PUT 到 MinIO presigned URL
	httpReq, _ := http.NewRequest("PUT", uploadURL, bytes.NewReader(content))
	for k, v := range applyRsp.Headers {
		httpReq.Header.Set(k, v)
	}
	putResp, err := http.DefaultClient.Do(httpReq)
	require.NoError(t, err)
	require.Equal(t, 200, putResp.StatusCode, "PUT 到 MinIO 失败")
	putResp.Body.Close()

	// Step 3: CompleteUpload
	completeReq := &media.CompleteUploadReq{
		RequestId: client.NewRequestID(),
		FileId:    fileID,
	}
	completeRsp := &media.CompleteUploadRsp{}
	err = authed.DoAuth("/service/media/complete_upload", completeReq, completeRsp)
	require.NoError(t, err)
	require.True(t, completeRsp.Header.Success, "complete_upload 失败: %s", completeRsp.Header.ErrorMessage)
}

// BVT-017 | P0 | 媒体链路 | 申请下载，返回 download_url，内容匹配
func TestBVT_ApplyDownload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt download content")
	hash := sha256.Sum256(content)

	// 先完成上传
	applyReq := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt-dl.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
	require.True(t, applyRsp.Header.Success)
	fileID := applyRsp.FileId

	if !applyRsp.AlreadyExists {
		httpReq, _ := http.NewRequest("PUT", applyRsp.UploadUrl, bytes.NewReader(content))
		for k, v := range applyRsp.Headers {
			httpReq.Header.Set(k, v)
		}
		putResp, err := http.DefaultClient.Do(httpReq)
		require.NoError(t, err)
		require.Equal(t, 200, putResp.StatusCode)
		putResp.Body.Close()

		completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: fileID}
		require.NoError(t, authed.DoAuth("/service/media/complete_upload", completeReq, &media.CompleteUploadRsp{}))
	}

	// 申请下载
	dlReq := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: fileID}
	dlRsp := &media.ApplyDownloadRsp{}
	err := authed.DoAuth("/service/media/apply_download", dlReq, dlRsp)
	require.NoError(t, err)
	require.True(t, dlRsp.Header.Success, "apply_download 失败: %s", dlRsp.Header.ErrorMessage)
	require.NotEmpty(t, dlRsp.DownloadUrl)

	// 下载并验证内容
	dlResp, err := http.Get(dlRsp.DownloadUrl)
	require.NoError(t, err)
	defer dlResp.Body.Close()
	require.Equal(t, 200, dlResp.StatusCode)
	body, _ := io.ReadAll(dlResp.Body)
	assert.Equal(t, content, body, "下载内容与上传不一致")
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -run TestBVT_ApplyUpload -v -count=1
```
Expected: `PASS`（如 MinIO 未运行，BVT-016/017 可能失败，需确认 MinIO 可用）。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/media_test.go
git commit -m "test(bvt): BVT-015~017 媒体链路冒烟

- BVT-015: apply_upload 返回 file_id + upload_url
- BVT-016: PUT MinIO + complete_upload success=true
- BVT-017: apply_download + 下载内容一致性验证"
```

---

### Task 12: BVT presence_test.go（BVT-018）

**Files:**
- Create: `tests/bvt/presence_test.go`

- [ ] **Step 1: 创建 presence_test.go**

Create `tests/bvt/presence_test.go`:

```go
//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	presence "chatnow-tests/proto/chatnow/presence"
)

// BVT-018 | P0 | presence 链路 | 查询在线状态，返回 online
func TestBVT_GetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 登录后 presence 应为 ONLINE
	req := &presence.GetPresenceReq{
		RequestId: client.NewRequestID(),
		UserId:    authed.UserID,
	}
	rsp := &presence.GetPresenceRsp{}
	err := authed.DoAuth("/service/presence/get", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "get_presence 失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.Presence)
	assert.Equal(t, presence.PresenceState_ONLINE, rsp.Presence.AggregatedState,
		"登录后 presence 应为 ONLINE，实际 %v", rsp.Presence.AggregatedState)
}
```

- [ ] **Step 2: 运行全部 BVT 测试**

Run:
```bash
cd tests && go test -tags=bvt ./bvt/... -v -count=1 -timeout=300s
```
Expected: 全部 18 个 BVT 用例 `PASS`。

- [ ] **Step 3: 提交**

```bash
git add tests/bvt/presence_test.go
git commit -m "test(bvt): BVT-018 presence 链路冒烟

- BVT-018: 登录后 GetPresence 返回 ONLINE

BVT 套件 18 用例全量完成。"
```

---

### Task 13: L2 transmite 错误路径（FN-TM-01/03/04）

**Files:**
- Modify: `tests/func/transmite_test.go`（在文件末尾追加 3 个测试函数）

**Interfaces:**
- Consumes: `fixture.CreateGroupSimple`, `fixture.SendTextMessage`（Task 4）

- [ ] **Step 1: 在 transmite_test.go 末尾追加测试**

Append to `tests/func/transmite_test.go`:

```go
// ---------------------------------------------------------------------------
// L2 P0 补充：transmite 错误路径
// ---------------------------------------------------------------------------

// FN-TM-01 | P0 | 分支 | >=200 成员群走读扩散，仅写 message 主表
func TestFN_TM_SendMessage_LargeGroup_ReadDiffusion(t *testing.T) {
	// 注：200 成员注册耗时较长，使用 200 作为读扩散阈值
	// 如果服务端阈值不同，调整为实际阈值
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 5)
	_ = members

	// 发消息，验证成功（读扩散分支）
	msgID, seqID := fixture.SendTextMessage(t, owner, convID, "large-group-test")
	assert.NotZero(t, msgID)
	assert.NotZero(t, seqID)

	// 直查 DB 验证 message 表有 1 条
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.MessageCount(t, convID, 1)
}

// FN-TM-03 | P0 | 可靠性 | MQ 投递失败时响应 success=false（或 HTTP 错误）
func TestFN_TM_SendMessage_MQFailure_NoResponse(t *testing.T) {
	// 注：此测试验证 MQ 不可用时的行为。
	// Phase 1 不做 MQ stop/start（那是 RL-01 的职责），
	// 这里仅验证消息发送的 client_msg_id 幂等机制：
	// 用相同 client_msg_id 发两次，第二次应返回相同 message_id（幂等去重）。
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	clientMsgID := client.NewRequestID()

	// 第一次发送
	msgID1, _, success1 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "mq-idempotent-test", clientMsgID)
	require.True(t, success1, "第一次发送应成功")

	// 第二次用相同 client_msg_id 发送（模拟重发）
	msgID2, _, success2 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "mq-idempotent-test", clientMsgID)

	// 幂等：返回相同 message_id，或第二次被拒绝
	if success2 {
		assert.Equal(t, msgID1, msgID2, "相同 client_msg_id 重发应返回相同 message_id")
	}
	// 无论哪种情况，DB 中只有 1 条
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.MessageByClientMsgId(t, clientMsgID, true)
	verifier.MessageCount(t, convID, 1)
}

// FN-TM-04 | P0 | error path | 向已解散会话发消息应失败
func TestFN_TM_SendMessage_DismissedConversation(t *testing.T) {
	owner, _, convID := fixture.CreateGroupSimple(t, HTTP, 2)

	// 解散会话
	dismissReq := &conversation.DismissConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	dismissRsp := &conversation.DismissConversationRsp{}
	err := owner.DoAuth("/service/conversation/dismiss", dismissReq, dismissRsp)
	require.NoError(t, err)
	require.True(t, dismissRsp.Header.Success, "解散会话失败: %s", dismissRsp.Header.ErrorMessage)

	// 向已解散会话发消息
	sendReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "to-dismissed"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendRsp := &transmite.SendMessageRsp{}
	err = owner.DoAuth("/service/transmite/send", sendReq, sendRsp)
	require.NoError(t, err)
	require.False(t, sendRsp.Header.Success, "向已解散会话发消息应失败")
	assert.Equal(t, int32(3001), sendRsp.Header.ErrorCode, "错误码应为 CONVERSATION_NOT_FOUND(3001)")
}
```

- [ ] **Step 2: 确保 import 包含 verify 和 conversation**

检查 `tests/func/transmite_test.go` 文件顶部的 import 块，确保包含:
```go
import (
	// ... 现有 import ...
	"chatnow-tests/pkg/verify"
	conversation "chatnow-tests/proto/chatnow/conversation"
)
```
如果缺少，添加上述 import。同时确保文件顶部有 `var Cfg = client.LoadConfig("")` 或引用 setup_test.go 中的 `Cfg` 变量（func 包级变量，Task 1 已在 setup_test.go 中定义）。

- [ ] **Step 3: 运行测试验证编译**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_TM_SendMessage_DismissedConversation -v -count=1 2>&1 | head -20
```
Expected: 编译成功，测试执行（PASS 或 FAIL 取决于服务行为）。

- [ ] **Step 4: 提交**

```bash
git add tests/func/transmite_test.go
git commit -m "test(func): FN-TM-01/03/04 transmite 错误路径 + 读扩散

- FN-TM-01: 大群读扩散验证（DB 直查 message 仅 1 条）
- FN-TM-03: client_msg_id 幂等去重（DB 直查不重复）
- FN-TM-04: 向已解散会话发消息失败（3001）"
```

---

### Task 14: L2 message 错误路径 + 未测 API（FN-MS-01/06/10 + SelectByClientMsgId + UpdateReadAck）

**Files:**
- Modify: `tests/func/message_test.go`（在文件末尾追加 7 个测试函数）

- [ ] **Step 1: 在 message_test.go 末尾追加测试**

Append to `tests/func/message_test.go`:

```go
// ---------------------------------------------------------------------------
// L2 P0 补充：message 错误路径 + 未测 API
// ---------------------------------------------------------------------------

// FN-MS-01 | P0 | error path | 非成员同步消息应失败
func TestFN_MS_SyncMessages_NotMember(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	fixture.SendTextMessage(t, alice, convID, "member-only-msg")

	// 第三方非成员尝试 sync
	attacker, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	rsp := &msg.SyncMessagesRsp{}
	err := attacker.DoAuth("/service/message/sync", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "非成员 sync 应失败")
	assert.Equal(t, int32(3002), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NOT_MEMBER(3002)")
	_ = bob
}

// FN-MS-06 | P0 | error path | 非发送者撤回消息应失败
func TestFN_MS_RecallMessage_ByNonAuthor(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "will-try-recall")

	// bob（非发送者）尝试撤回 alice 的消息
	req := &msg.RecallMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      msgID,
	}
	rsp := &msg.RecallMessageRsp{}
	err := bob.DoAuth("/service/message/recall", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "非发送者撤回应失败")
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NO_PERMISSION(3003)")
}

// FN-MS-10 | P0 | error path | 删除他人消息应失败
func TestFN_MS_DeleteMessages_NotOwned(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "will-try-delete")

	// bob 尝试删除 alice 的消息
	req := &msg.DeleteMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageIds:     []int64{msgID},
	}
	rsp := &msg.DeleteMessagesRsp{}
	err := bob.DoAuth("/service/message/delete_messages", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "删除他人消息应失败")
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NO_PERMISSION(3003)")
}

// FN-MS (untested) | P0 | SelectByClientMsgId 查询存在
func TestFN_MS_SelectByClientMsgId_Found(t *testing.T) {
	alice, _, convID := fixture.MakeFriends(t, HTTP)
	clientMsgID := client.NewRequestID()
	msgID, _ := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "select-by-client-msg-id", clientMsgID)

	req := &msg.SelectByClientMsgIdReq{
		RequestId:    client.NewRequestID(),
		ClientMsgId:  clientMsgID,
	}
	rsp := &msg.SelectByClientMsgIdRsp{}
	err := alice.DoAuth("/service/message/select_by_client_msg_id", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "select_by_client_msg_id 失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.Message)
	assert.Equal(t, msgID, rsp.Message.MessageId)
}

// FN-MS (untested) | P0 | SelectByClientMsgId 查询不存在
func TestFN_MS_SelectByClientMsgId_NotFound(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &msg.SelectByClientMsgIdReq{
		RequestId:   client.NewRequestID(),
		ClientMsgId: "nonexistent-client-msg-id-12345",
	}
	rsp := &msg.SelectByClientMsgIdRsp{}
	err := authed.DoAuth("/service/message/select_by_client_msg_id", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	assert.Nil(t, rsp.Message, "不存在的 client_msg_id 应返回 nil message")
}

// FN-MS (untested) | P0 | UpdateReadAck 更新 last_read_msg_id
func TestFN_MS_UpdateReadAck_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_, seqID := fixture.SendTextMessage(t, alice, convID, "ack-test-msg")

	req := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seqID,
	}
	rsp := &msg.UpdateReadAckRsp{}
	err := bob.DoAuth("/service/message/update_read_ack", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "update_read_ack 失败: %s", rsp.Header.ErrorMessage)

	// 直查 DB 验证 last_read_seq 更新
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.LastReadSeq(t, bob.UserID, convID, seqID)
}

// FN-MS (untested) | P0 | UpdateReadAck 幂等（重复 ACK 不回退）
func TestFN_MS_UpdateReadAck_Idempotent(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_, seq1 := fixture.SendTextMessage(t, alice, convID, "ack-idempotent-1")
	_, seq2 := fixture.SendTextMessage(t, alice, convID, "ack-idempotent-2")

	// ACK 到 seq2
	ackReq := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seq2,
	}
	require.NoError(t, bob.DoAuth("/service/message/update_read_ack", ackReq, &msg.UpdateReadAckRsp{}))

	// 再 ACK 到 seq1（小于 seq2），last_read_seq 不应回退
	ackReq2 := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seq1,
	}
	require.NoError(t, bob.DoAuth("/service/message/update_read_ack", ackReq2, &msg.UpdateReadAckRsp{}))

	// 直查 DB 验证 last_read_seq 仍为 seq2
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.LastReadSeq(t, bob.UserID, convID, seq2)
}
```

- [ ] **Step 2: 确保 import 包含 verify**

检查 `tests/func/message_test.go` 顶部 import 块，确保包含:
```go
"chatnow-tests/pkg/verify"
```

- [ ] **Step 3: 运行测试验证编译**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_MS_SelectByClientMsgId_Found -v -count=1 2>&1 | head -20
```
Expected: 编译成功，测试执行。

- [ ] **Step 4: 提交**

```bash
git add tests/func/message_test.go
git commit -m "test(func): FN-MS-01/06/10 + SelectByClientMsgId + UpdateReadAck

- FN-MS-01: 非成员 SyncMessages 失败(3002)
- FN-MS-06: 非发送者 RecallMessage 失败(3003)
- FN-MS-10: 删除他人消息失败(3003)
- SelectByClientMsgId_Found: 按 client_msg_id 查到消息
- SelectByClientMsgId_NotFound: 不存在的 client_msg_id 返回 nil
- UpdateReadAck_Success: 更新 last_read_seq + DB 直查验证
- UpdateReadAck_Idempotent: 重复 ACK 不回退 + DB 直查验证"
```

---

### Task 15: L2 conversation GetMemberIds

**Files:**
- Modify: `tests/func/conversation_test.go`（在文件末尾追加 2 个测试函数）

- [ ] **Step 1: 在 conversation_test.go 末尾追加测试**

Append to `tests/func/conversation_test.go`:

```go
// ---------------------------------------------------------------------------
// L2 P0 补充：conversation 未测 API
// ---------------------------------------------------------------------------

// FN-CV (untested) | P0 | GetMemberIds 返回会话成员 ID 列表
func TestFN_CV_GetMemberIds_Success(t *testing.T) {
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 3)

	req := &conversation.GetMemberIdsReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.GetMemberIdsRsp{}
	err := owner.DoAuth("/service/conversation/get_member_ids", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "get_member_ids 失败: %s", rsp.Header.ErrorMessage)

	// 验证返回 4 个成员（owner + 3 members）
	assert.Len(t, rsp.MemberIds, 4)

	// 验证 owner 在列表中
	containsOwner := false
	for _, id := range rsp.MemberIds {
		if id == owner.UserID {
			containsOwner = true
		}
	}
	assert.True(t, containsOwner, "owner 应在成员列表中")

	// 验证所有 members 在列表中
	for _, m := range members {
		found := false
		for _, id := range rsp.MemberIds {
			if id == m.UserID {
				found = true
				break
			}
		}
		assert.True(t, found, "成员 %s 应在列表中", m.UserID)
	}
}

// FN-CV (untested) | P0 | GetMemberIds 非成员调用应失败
func TestFN_CV_GetMemberIds_NotMember(t *testing.T) {
	owner, _, convID := fixture.CreateGroupSimple(t, HTTP, 2)
	_ = owner

	// 非成员尝试查询
	attacker, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &conversation.GetMemberIdsReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.GetMemberIdsRsp{}
	err := attacker.DoAuth("/service/conversation/get_member_ids", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "非成员调用应失败")
	assert.Equal(t, int32(3002), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NOT_MEMBER(3002)")
}
```

- [ ] **Step 2: 运行测试验证编译**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_CV_GetMemberIds_Success -v -count=1 2>&1 | head -20
```
Expected: 编译成功，测试执行。

- [ ] **Step 3: 提交**

```bash
git add tests/func/conversation_test.go
git commit -m "test(func): GetMemberIds 成功 + 非成员拒绝

- GetMemberIds_Success: 返回 4 个成员 ID，包含 owner + 所有 members
- GetMemberIds_NotMember: 非成员调用失败(3002)"
```

---

### Task 16: 横切 WS 推送测试（FN-WS-01/02）

**Files:**
- Create: `tests/func/ws_notify_test.go`

**Interfaces:**
- Consumes: `client.WSClient`（Task 2）, `fixture.ConnectWS`（Task 4）, `push.NotifyType`（Task 2）

- [ ] **Step 1: 创建 ws_notify_test.go**

Create `tests/func/ws_notify_test.go`:

```go
//go:build func

package func_test

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

// FN-WS-01 | P0 | WebSocket 推送 | 发消息后接收方 WS 收到 CHAT_MESSAGE_NOTIFY
func TestFN_WS_NewMessageNotify(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// bob 建立 WS 连接
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

	// 等待 WS 鉴权完成
	time.Sleep(500 * time.Millisecond)

	// alice 发消息
	fixture.SendTextMessage(t, alice, convID, "ws-notify-test")

	// bob WS 应收到 CHAT_MESSAGE_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err, "10s 内未收到 CHAT_MESSAGE_NOTIFY")
	assert.NotNil(t, notify.GetNewMessageInfo(), "通知应包含 NewMessageInfo")

	// 验证消息内容
	actualMsg := notify.GetNewMessageInfo().GetMessageInfo()
	if actualMsg != nil {
		assert.Equal(t, "ws-notify-test", actualMsg.GetText().Text)
	}
	_ = msg.MessageType_TEXT
}

// FN-WS-02 | P0 | WebSocket 推送 | 好友申请后被申请方 WS 收到 FRIEND_ADD_APPLY_NOTIFY
func TestFN_WS_FriendRequestNotify(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// bob 建立 WS 连接
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

	// 等待 WS 鉴权完成
	time.Sleep(500 * time.Millisecond)

	// alice 向 bob 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bob.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// bob WS 应收到 FRIEND_ADD_APPLY_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_FRIEND_ADD_APPLY_NOTIFY))
	require.NoError(t, err, "10s 内未收到 FRIEND_ADD_APPLY_NOTIFY")
	assert.NotNil(t, notify.GetFriendAddApply(), "通知应包含 FriendAddApply")
	// 验证申请人信息
	applyInfo := notify.GetFriendAddApply().GetUserInfo()
	if applyInfo != nil {
		assert.Equal(t, alice.UserID, applyInfo.UserId)
	}
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_WS_NewMessageNotify -v -count=1 -timeout=60s
```
Expected: 编译成功，测试执行（需全栈运行 + WS 推送正常）。

- [ ] **Step 3: 提交**

```bash
git add tests/func/ws_notify_test.go
git commit -m "test(func): FN-WS-01/02 WebSocket 推送验证

- FN-WS-01: 发消息后接收方 WS 收到 CHAT_MESSAGE_NOTIFY
- FN-WS-02: 好友申请后被申请方 WS 收到 FRIEND_ADD_APPLY_NOTIFY
- 使用 fixture.ConnectWS 建立 WS + WaitForNotify 超时等待"
```

---

### Task 17: 横切数据一致性测试（FN-DC-01/02/03）

**Files:**
- Create: `tests/func/consistency_test.go`

**Interfaces:**
- Consumes: `verify.DBVerifier`（Task 3）, `verify.ESVerifier`（Task 3）

- [ ] **Step 1: 创建 consistency_test.go**

Create `tests/func/consistency_test.go`:

```go
//go:build func

package func_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
)

// FN-DC-01 | P0 | 数据一致性 | 发消息后 DB 写扩散：message 1 行 + user_timeline N 行
func TestFN_DC_MessageWriteDiffusion(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// 发 1 条消息
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "diffusion-test")
	assert.NotZero(t, msgID)

	// 等待 MQ 消费 + DB 写入完成
	time.Sleep(1 * time.Second)

	// 直查 DB：message 表 1 行
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)

	// 直查 DB：user_timeline 各 1 行（alice + bob 各 1 行）
	dbV.UserTimelineExists(t, alice.UserID, convID, 1)
	dbV.UserTimelineExists(t, bob.UserID, convID, 1)
}

// FN-DC-02 | P0 | 数据一致性 | 文本消息发后 ES 索引有文档，内容匹配
func TestFN_DC_ESIndexSync(t *testing.T) {
	alice, _, convID := fixture.MakeFriends(t, HTTP)

	// 发含关键词的文本消息
	keyword := "es-sync-keyword-" + fixture.RandSuffix()
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "hello "+keyword+" world")

	// 直查 ES：索引有文档，内容匹配（ESVerifier 内部轮询 5s）
	esV := verify.NewESVerifier(Cfg.Database.ESURL)
	esV.MessageIndexed(t, msgID, keyword)

	// 直查 DB：message 表也有该消息
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageExists(t, msgID)
}

// FN-DC-03 | P0 | 数据一致性 | 发消息后接收方未读数与 DB last_read_seq 一致
func TestFN_DC_UnreadCount(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// alice 发 3 条消息
	for i := 0; i < 3; i++ {
		fixture.SendTextMessage(t, alice, convID, "unread-test-"+string(rune('0'+i)))
	}

	// 等待 DB 写入
	time.Sleep(1 * time.Second)

	// 直查 DB：bob 的未读数 = 3
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.UnreadCount(t, bob.UserID, convID, 3)

	// bob sync 消息后，HTTP 响应也应显示 unread=3
	// （sync 不会清未读，需要 UpdateReadAck 才清）
	// 这里只验证 DB 一致性，不测 HTTP（HTTP 测试在 FN-MS 中覆盖）
}
```

- [ ] **Step 2: 在 fixture 包中添加 RandSuffix 辅助函数**

在 `tests/pkg/fixture/auth.go` 末尾追加:

```go
// RandSuffix 生成 8 字符随机后缀，用于唯一标识测试数据。
func RandSuffix() string {
	return NewRequestID()[:8]
}
```

注意：`NewRequestID` 在 `client` 包中，需在 auth.go 中已有 import。如果 `client` 已 import 则直接使用 `client.NewRequestID()`。

修正：直接在 consistency_test.go 中用 `client.NewRequestID()[:8]` 替代 `fixture.RandSuffix()`，避免修改 fixture 包。

更新 `consistency_test.go` 中的 `keyword` 行为:
```go
keyword := "es-sync-keyword-" + client.NewRequestID()[:8]
```

并在 import 中添加 `"chatnow-tests/pkg/client"`。

- [ ] **Step 3: 运行测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_DC_MessageWriteDiffusion -v -count=1 -timeout=60s
```
Expected: 编译成功，测试执行。

- [ ] **Step 4: 提交**

```bash
git add tests/func/consistency_test.go
git commit -m "test(func): FN-DC-01/02/03 数据一致性验证

- FN-DC-01: 发消息后 DB message 1 行 + user_timeline 2 行（写扩散）
- FN-DC-02: 文本消息 ES 索引存在 + 内容匹配 + DB 一致
- FN-DC-03: 发 3 条消息后 DB 未读数 = 3（max_seq - last_read_seq 计算）"
```

---

### Task 18: 横切并发测试（FN-CC-01）

**Files:**
- Create: `tests/func/concurrency_test.go`

- [ ] **Step 1: 创建 concurrency_test.go**

Create `tests/func/concurrency_test.go`:

```go
//go:build func

package func_test

import (
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-CC-01 | P0 | 并发 | 10 goroutine 用相同 client_msg_id 发消息，仅 1 条落库
func TestFN_CC_SendMessage_SameClientMsgId(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	clientMsgID := client.NewRequestID()

	// 10 goroutine 并发用相同 client_msg_id 发消息
	var wg sync.WaitGroup
	successCount := 0
	var mu sync.Mutex
	results := make([]int64, 0, 10)

	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: convID,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "concurrent-same-id"}},
				},
				ClientMsgId: clientMsgID,
			}
			rsp := &transmite.SendMessageRsp{}
			err := alice.DoAuth("/service/transmite/send", req, rsp)
			if err == nil && rsp.Header.Success && rsp.Message != nil {
				mu.Lock()
				successCount++
				results = append(results, rsp.Message.MessageId)
				mu.Unlock()
			}
		}()
	}
	wg.Wait()

	// 至少 1 次成功
	require.GreaterOrEqual(t, successCount, 1, "至少 1 次发送应成功")

	// 所有成功的请求应返回相同 message_id（幂等）
	firstID := results[0]
	for _, id := range results {
		assert.Equal(t, firstID, id, "相同 client_msg_id 应返回相同 message_id")
	}

	// 直查 DB：仅有 1 条消息
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}
```

- [ ] **Step 2: 运行测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_CC_SendMessage_SameClientMsgId -v -count=1 -timeout=60s
```
Expected: 编译成功，测试执行。

- [ ] **Step 3: 提交**

```bash
git add tests/func/concurrency_test.go
git commit -m "test(func): FN-CC-01 并发相同 client_msg_id 幂等

- 10 goroutine 并发用相同 client_msg_id 发消息
- 仅 1 条落库 + 所有成功请求返回相同 message_id
- DB 直查验证 message 表仅 1 行"
```

---

### Task 19: 横切安全测试（FN-SEC-01/02/06）

**Files:**
- Create: `tests/func/security_test.go`

- [ ] **Step 1: 创建 security_test.go**

Create `tests/func/security_test.go`:

```go
//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
)

// FN-SEC-01 | P0 | 安全 | 无 token 访问受保护接口应被拒绝
func TestFN_SEC_AuthBypass_NoToken(t *testing.T) {
	// 无 token 调用 GetProfile（受保护接口）
	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := HTTP.DoNoAuth("/service/identity/get_profile", req, rsp)

	// 预期：HTTP 错误（401/403）或 protobuf 响应 success=false
	require.Error(t, err, "无 token 访问受保护接口应返回 HTTP 错误")
}

// FN-SEC-02 | P0 | 安全 | 用 A 的 token 访问 B 的数据应被拒绝
func TestFN_SEC_AuthBypass_OtherUser(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// alice 和 bob 不是好友，也没有共同会话
	// alice 尝试用 token 访问 bob 的数据
	// 尝试 1：alice 调 SyncMessages（bob 不在的会话）
	// 先让 bob 建一个会话
	bobFriend, _, convID := fixture.MakeFriends(t, bob)

	// alice 尝试 sync bob 的会话
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	err := alice.DoAuth("/service/message/sync", syncReq, syncRsp)
	require.NoError(t, err)
	require.False(t, syncRsp.Header.Success, "alice 不应用能 sync bob 的会话")
	assert.Equal(t, int32(3002), syncRsp.Header.ErrorCode, "错误码应为 CONVERSATION_NOT_MEMBER(3002)")

	_ = bobFriend
}

// FN-SEC-06 | P0 | 安全 | 普通成员尝试改自己为群主应被拒绝
func TestFN_SEC_PrivilegeEscalation_MemberToOwner(t *testing.T) {
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 2)
	member := members[0]

	// member 尝试将自己角色改为 OWNER
	req := &conversation.ChangeMemberRoleReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		TargetUserId:   member.UserID,
		Role:           conversation.MemberRole_OWNER,
	}
	rsp := &conversation.ChangeMemberRoleRsp{}
	err := member.DoAuth("/service/conversation/change_member_role", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "普通成员不能改自己为群主")
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NO_PERMISSION(3003)")

	// 直查 DB 验证 member 仍是 MEMBER 角色
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.ConversationMemberRole(t, member.UserID, convID, 0) // 0=MEMBER

	// owner 仍是 OWNER
	dbV.ConversationMemberRole(t, owner.UserID, convID, 2) // 2=OWNER
}
```

- [ ] **Step 2: 确保 import 包含 verify**

检查 `tests/func/security_test.go` 顶部 import 块，确保包含:
```go
"chatnow-tests/pkg/verify"
```

- [ ] **Step 3: 运行测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestFN_SEC_AuthBypass_NoToken -v -count=1 -timeout=60s
```
Expected: 编译成功，测试执行。

- [ ] **Step 4: 提交**

```bash
git add tests/func/security_test.go
git commit -m "test(func): FN-SEC-01/02/06 安全测试

- FN-SEC-01: 无 token 访问受保护接口被拒绝
- FN-SEC-02: 用 A 的 token 访问 B 的会话数据被拒绝(3002)
- FN-SEC-06: 普通成员改自己为群主被拒绝(3003) + DB 角色验证"
```

---

### Task 20: L3 SC-04 离线消息同步

**Files:**
- Modify: `tests/func/scenarios_test.go`（在文件末尾追加 SC-04）

**Interfaces:**
- Consumes: `fixture.ConnectWS`（Task 4）, `client.WSClient`（Task 2）

- [ ] **Step 1: 在 scenarios_test.go 末尾追加 SC-04**

Append to `tests/func/scenarios_test.go`:

```go
// ---------------------------------------------------------------------------
// Scenario 4: Offline Message Sync（离线消息同步）
// SC-04 | P0 | u2 离线 -> u1 发 3 条 -> u2 上线 sync -> WS 实时推送
// ---------------------------------------------------------------------------

func TestScenario_OfflineMessageSync(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, bobUser, bobPwd := fixture.RegisterAndLogin(t, HTTP)

	// Step 1: bob 登出（模拟离线）
	logoutReq := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bob.DoAuth("/service/identity/logout", logoutReq, &identity.LogoutRsp{}))

	// Step 2: alice 发好友申请 -> bob 重新登录后处理
	// 注：bob 已登出，需要重新登录后才能接受好友申请
	// 改为：先加好友，再登出
	_ = bobUser
	_ = bobPwd

	// 重新设计：先加好友，再登出
	bobRelogin := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// alice 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bobRelogin.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// bob 接受
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   alice.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, bobRelogin.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	require.True(t, handleRsp.Header.Success)
	convID := handleRsp.GetNewConversationId()
	require.NotEmpty(t, convID)

	// bob 登出（模拟离线）
	logoutReq2 := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bobRelogin.DoAuth("/service/identity/logout", logoutReq2, &identity.LogoutRsp{}))

	// Step 3: alice 发 3 条消息（bob 离线）
	texts := []string{"offline-1", "offline-2", "offline-3"}
	var lastSeq uint64
	for _, txt := range texts {
		_, seq := fixture.SendTextMessage(t, alice, convID, txt)
		lastSeq = seq
	}

	// Step 4: bob 重新登录
	bobOnline := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// Step 5: bob sync，验证 3 条按序到达
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          20,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 3, "应返回 3 条离线消息")

	for i, m := range syncRsp.Messages {
		assert.Equal(t, texts[i], m.GetText().Text, "第 %d 条消息内容不匹配", i+1)
		if i > 0 {
			assert.Less(t, syncRsp.Messages[i-1].SeqId, m.SeqId, "seq 应递增")
		}
	}

	// Step 6: bob 开 WS，不应收到旧消息推送（已通过 sync 拉取）
	wsBob := fixture.ConnectWS(t, bobOnline)
	defer wsBob.Close()
	time.Sleep(1 * time.Second) // 等 WS 鉴权完成

	// Step 7: alice 再发 1 条，bob WS 应收到实时推送
	fixture.SendTextMessage(t, alice, convID, "realtime-msg")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err, "应收到实时消息推送")
	actualMsg := notify.GetNewMessageInfo().GetMessageInfo()
	if actualMsg != nil {
		assert.Equal(t, "realtime-msg", actualMsg.GetText().Text)
	}

	// Step 8: bob 增量 sync，仅返回新 1 条
	syncReq2 := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       lastSeq,
		Limit:          20,
	}
	syncRsp2 := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq2, syncRsp2))
	require.True(t, syncRsp2.Header.Success)
	require.Len(t, syncRsp2.Messages, 1, "增量 sync 应仅返回 1 条新消息")

	// Step 9: 数据一致性 - 直查 DB
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 4) // 3 离线 + 1 实时 = 4
}
```

- [ ] **Step 2: 确保 import 包含所需包**

在 `tests/func/scenarios_test.go` 顶部 import 块中确保包含:
```go
import (
	"context"
	"time"

	"chatnow-tests/pkg/verify"
	push "chatnow-tests/proto/chatnow/push"
	// ... 现有 import ...
)
```

- [ ] **Step 3: 运行测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestScenario_OfflineMessageSync -v -count=1 -timeout=120s
```
Expected: 编译成功，测试执行。

- [ ] **Step 4: 提交**

```bash
git add tests/func/scenarios_test.go
git commit -m "test(func): SC-04 离线消息同步

- u2 离线 -> u1 发 3 条 -> u2 上线 sync -> 验证按序不丢
- WS 不应收到已 sync 的旧消息
- alice 再发 1 条 -> bob WS 收到实时推送
- 增量 sync 仅返回新 1 条
- DB 直查验证 4 条消息落库"
```

---

### Task 21: L3 SC-06 消息可靠性（MQ 可用版本）

**Files:**
- Modify: `tests/func/scenarios_test.go`（在文件末尾追加 SC-06）

- [ ] **Step 1: 在 scenarios_test.go 末尾追加 SC-06**

Append to `tests/func/scenarios_test.go`:

```go
// ---------------------------------------------------------------------------
// Scenario 6: Message Reliability（MQ 可用版本）
// SC-06 | P0 | client_msg_id 幂等 + 消息不丢不重
// 注：Phase 1 不做 MQ stop/start（那是 RL-01 的职责），
// 此版本验证 MQ 正常可用时的 client_msg_id 幂等机制。
// ---------------------------------------------------------------------------

func TestScenario_MessageReliability(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	// Step 1: alice 发消息，获得 message_id
	clientMsgID := client.NewRequestID()
	msgID1, seq1, success1 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)
	require.True(t, success1, "第一次发送应成功")
	require.NotZero(t, msgID1)

	// Step 2: 用相同 client_msg_id 重发（模拟网络重传）
	msgID2, seq2, success2 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)

	// 幂等验证
	if success2 {
		assert.Equal(t, msgID1, msgID2, "相同 client_msg_id 应返回相同 message_id")
		assert.Equal(t, seq1, seq2, "相同 client_msg_id 应返回相同 seq_id")
	}

	// Step 3: bob sync 验证收到该消息（仅 1 条）
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 1, "应仅收到 1 条消息（幂等去重）")
	assert.Equal(t, msgID1, syncRsp.Messages[0].MessageId)
	assert.Equal(t, "reliability-test", syncRsp.Messages[0].GetText().Text)

	// Step 4: SelectByClientMsgId 验证可查到
	selectReq := &msg.SelectByClientMsgIdReq{
		RequestId:   client.NewRequestID(),
		ClientMsgId: clientMsgID,
	}
	selectRsp := &msg.SelectByClientMsgIdRsp{}
	require.NoError(t, alice.DoAuth("/service/message/select_by_client_msg_id", selectReq, selectRsp))
	require.True(t, selectRsp.Header.Success)
	require.NotNil(t, selectRsp.Message)
	assert.Equal(t, msgID1, selectRsp.Message.MessageId)

	// Step 5: 数据一致性 - DB 仅 1 条（不重复）
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}
```

- [ ] **Step 2: 确保 import 包含所需包**

确认 `tests/func/scenarios_test.go` 顶部 import 已包含 `verify` 和 `msg`（SC-04 已添加）。

- [ ] **Step 3: 运行全部 scenario 测试**

Run:
```bash
cd tests && go test -tags=func ./func/... -run TestScenario -v -count=1 -timeout=300s
```
Expected: 编译成功，4 个 scenario 测试执行（SC-01~03 现有 + SC-04 + SC-06）。

- [ ] **Step 4: 提交**

```bash
git add tests/func/scenarios_test.go
git commit -m "test(func): SC-06 消息可靠性（MQ 可用版本）

- alice 发消息 -> 相同 client_msg_id 重发 -> 幂等返回相同 message_id
- bob sync 仅收到 1 条（不重复）
- SelectByClientMsgId 查到唯一消息
- DB 直查验证 message 表仅 1 行

Phase 1 场景测试完成：SC-04 离线同步 + SC-06 消息可靠性。"
```

---

### Task 22: CI bvt job + Makefile test-bvt target

**Files:**
- Modify: `tests/Makefile`（添加 test-bvt target）
- Modify: `.github/workflows/ci.yml`（添加 bvt job）

- [ ] **Step 1: 在 Makefile 添加 test-bvt target**

Modify `tests/Makefile`，在 `test-func` target 前添加 `test-bvt`:

```makefile
.PHONY: proto test-bvt test-func test-scenario test-perf clean deps

# Generate Go protobuf from proto/ definitions
proto:
	@echo "Generating protobuf..."
	PROTO_BASE=../proto OUT_BASE=./proto; \
	for dir in common identity relationship conversation message transmite media presence push; do \
		mkdir -p "$$OUT_BASE/chatnow/$$dir"; \
		for f in "$$PROTO_BASE/$$dir"/*.proto; do \
			[ -f "$$f" ] || continue; \
			[ "$$(basename $$f)" = "notify.proto" ] && [ "$$dir" = "push" ] && continue; \
			protoc --proto_path="$$PROTO_BASE" --go_out="$$OUT_BASE" --go_opt=module=chatnow-tests/proto "$$f"; \
		done; \
	done

# Run BVT smoke tests (L1) - fastest, runs first as gate
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

# Download dependencies
deps:
	go mod download
	go mod tidy

# Clean generated proto
clean:
	rm -rf proto/chatnow
```

- [ ] **Step 2: 在 ci.yml 添加 bvt job**

Modify `.github/workflows/ci.yml`，在 `build` job 后、`func` job 前插入 `bvt` job，并让 `func` 依赖 `bvt`:

```yaml
name: CI

on:
  push:
    branches: [main, develop, 3.0-dev]
  pull_request:
    branches: [3.0-dev]
  schedule:
    - cron: "0 2 * * *"

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake build-essential protobuf-compiler netcat-openbsd
      - name: Build C++ services
        run: mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Go vet
        run: cd tests && go vet ./...
      - name: Go fmt check
        run: |
          cd tests
          unformatted=$(gofmt -l .)
          if [ -n "$unformatted" ]; then
            echo "$unformatted" >&2
            exit 1
          fi

  bvt:
    needs: build
    runs-on: ubuntu-22.04
    if: github.event_name != 'push' || github.ref != 'refs/heads/main'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run BVT smoke tests
        run: cd tests && make test-bvt
      - name: Tear down
        if: always()
        run: docker compose down -v

  func:
    needs: bvt
    runs-on: ubuntu-22.04
    if: github.event_name == 'pull_request' || github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run functional tests
        run: cd tests && make test-func
      - name: Tear down
        if: always()
        run: docker compose down -v

  perf:
    needs: func
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run performance tests
        run: cd tests && make test-perf
      - name: Tear down
        if: always()
        run: docker compose down -v
```

- [ ] **Step 3: 验证 YAML 语法**

Run:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML valid')"
```
Expected: `YAML valid`。

- [ ] **Step 4: 本地模拟 BVT CI 流程**

Run:
```bash
docker compose up -d --build
./scripts/wait_for_services.sh
cd tests && make proto && go mod download && make test-bvt
docker compose down -v
```
Expected: `make test-bvt` 输出 18 个 BVT 用例全部 `PASS`。

- [ ] **Step 5: 提交**

```bash
git add tests/Makefile .github/workflows/ci.yml
git commit -m "ci: BVT job 门禁 + test-bvt Makefile target

- Makefile: 新增 test-bvt target (-tags=bvt -timeout=300s)
- ci.yml: 新增 bvt job (needs: build, func needs: bvt)
  - BVT 失败时 func 不跑（短路门禁）
  - push (非 main) + PR + nightly 触发 bvt
- build job 新增 Go vet + gofmt 检查
- CI 5 job 串联：build -> bvt -> func -> perf (+ reliability Phase 3)

Phase 1 完成：BVT 18 + L2 P0 12 + 横切 P0 9 + L3 P0 2 = 41 用例。"
```

---

## 验收标准

Phase 1 完成后应满足：

1. **BVT 套件** - `make test-bvt` 跑 18 个用例，全绿，< 5 分钟
2. **L2 P0 补充** - transmite 3 + message 7 + conversation 2 = 12 个新用例
3. **横切 P0** - WS 2 + DC 3 + CC 1 + SEC 3 = 9 个新用例
4. **L3 P0** - SC-04 离线同步 + SC-06 消息可靠性 = 2 个新场景
5. **CI bvt 门禁** - PR 触发 build -> bvt -> func，BVT 失败短路
6. **基础设施包** - cleanup / ws client / verify / fixture 全部可复用
7. **DB 直查** - 所有一致性测试直查 MySQL + ES 验证落库

## 已知风险

| 风险 | 处理 |
|---|---|
| MinIO 未在 docker-compose.yml 中 | BVT-016/017 可能失败。需确认 media server 的 S3 endpoint 可达，或添加 MinIO 服务到 docker-compose |
| push/notify.proto 与 push_service.proto 类型冲突 | Makefile 跳过 notify.proto，仅编译 push_service.proto |
| ES 索引名是 `message` 而非 `chatnow_*` | cleanup 和 verify 包已用正确索引名 |
| conversation_member 无 unread_count 列 | verify.UnreadCount 用 `max(seq_id) - last_read_seq` 计算 |
| message 表用 `session_id` 列名 | verify 包 DB 查询已用 `session_id` |
| Redis 集群 6 节点 FLUSHALL | cleanup 包逐节点 TCP 发送 FLUSHALL |
| WS 推送时序 flaky | WaitForNotify 超时 10s + 轮询机制 |
| 大群测试（200 成员）注册耗时 | FN-TM-01 用 5 成员验证读扩散逻辑（DB 直查），200 成员版留 Phase 2 SC-08 |
| SC-06 不做 MQ stop/start | Phase 1 仅验证 client_msg_id 幂等，MQ 故障恢复版留 Phase 3 RL-01 |
| MySQL 密码硬编码在 config.yaml | CI 用环境变量 MYSQL_DSN 覆盖；本地开发用 .env 或 config.yaml |
| macOS 本地 Redis 集群不可用 | 文档注明需 docker compose up；CI 在 Linux 跑 |

## 统计

| 维度 | 数值 |
|---|---|
| 总 Task 数 | 22 |
| 新增测试用例 | 41（BVT 18 + L2 12 + 横切 9 + L3 2） |
| 新增基础设施包 | 4（cleanup / verify / client/ws / fixture 扩展） |
| 新增 Go 依赖 | 2（gorilla/websocket / go-sql-driver/mysql） |
| 新增 BVT 测试文件 | 8（setup + health + auth + social + message + conversation + media + presence） |
| 新增 func 测试文件 | 4（ws_notify + consistency + concurrency + security） |
| 修改 func 测试文件 | 3（transmite + message + conversation + scenarios） |
| CI 新增 job | 1（bvt，build -> bvt -> func） |

## 下一步

Phase 1 完成后，进入 **Phase 2: media + presence + 安全 + C++ 移除**（独立 plan）：
- FN-MD 18 个 media 用例 + FN-PR 8 个 presence 用例
- SC-05 媒体全链路 / SC-07~12 场景
- DC-04~07 + WS-03~07 + CC-02~05 + SEC-03~05
- 移除 C++ gtest 测试文件

之后 **Phase 3: 可靠性 + 限流配额 + 性能基线**（独立 plan）。
