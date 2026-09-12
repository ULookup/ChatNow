package cleanup

import (
	"bufio"
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
		{"Push", "127.0.0.1:10008"},
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
	s, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil {
		return fmt.Errorf("read FLUSHALL response: %w", err)
	}
	// Replica contents converge from the primaries flushed by the same suite.
	// Authentication, loading, and other failures still abort cleanup.
	if strings.HasPrefix(s, "-READONLY ") {
		return nil
	}
	if !strings.HasPrefix(s, "+OK") {
		return fmt.Errorf("FLUSHALL failed: %s", strings.TrimSpace(s))
	}
	return nil
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

// EtcdServiceCount 查询 etcd 中 /service/ 前缀下注册的服务数（BVT-003 用）。
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
