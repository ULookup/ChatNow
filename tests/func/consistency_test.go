//go:build func

package func_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
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
	keyword := "es-sync-keyword-" + client.NewRequestID()[:8]
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "hello "+keyword+" world")
	require.NotZero(t, msgID)

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
