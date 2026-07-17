package verify

import (
	"database/sql"
	"fmt"
	"testing"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

// DBVerifier 直查 MySQL 验证 HTTP 响应与底层存储一致。
type DBVerifier struct {
	db *sql.DB
}

type MediaFileRecord struct {
	Bucket    string
	ObjectKey string
	Status    int
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

// WaitMessageExists waits for the asynchronous MQ consumer to persist a message.
func (v *DBVerifier) WaitMessageExists(t testing.TB, messageID int64, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		var count int
		err := v.db.QueryRow("SELECT COUNT(*) FROM message WHERE message_id = ?", messageID).Scan(&count)
		if err == nil && count == 1 {
			return
		}
		if err != nil {
			t.Fatalf("query message %d while waiting: %v", messageID, err)
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("message %d was not persisted within %s", messageID, timeout)
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

// LastAckSeq verifies the delivery acknowledgement cursor.
func (v *DBVerifier) LastAckSeq(t testing.TB, userID, conversationID string, expected uint64) {
	var seq uint64
	err := v.db.QueryRow(
		"SELECT last_ack_seq FROM conversation_member WHERE user_id = ? AND conversation_id = ?",
		userID, conversationID,
	).Scan(&seq)
	if err != nil {
		t.Fatalf("query last_ack_seq: %v", err)
	}
	if seq != expected {
		t.Fatalf("last_ack_seq user=%s conv=%s 期望 %d，实际 %d", userID, conversationID, expected, seq)
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
