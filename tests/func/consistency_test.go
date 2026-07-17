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
	msg "chatnow-tests/proto/chatnow/message"
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
	// （sync 与送达 ACK 都不会清未读；用户已读需调用 Conversation.MarkRead）
	// 这里只验证 DB 一致性，不测 HTTP（HTTP 测试在 FN-MS 中覆盖）
}

// FN-DC-04 | P1 | consistency | 撤回后直查 DB：message.status=RECALLED，timeline 不删
func TestFN_DC_RecallMessage(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "will-recall-for-dc")

	// 等待 MQ 消费 + DB 写入完成
	time.Sleep(1 * time.Second)

	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()

	// 撤回前直查 DB：status=NORMAL(0)
	dbV.MessageStatus(t, mID, 0)

	// 撤回
	recallReq := &msg.RecallMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	require.NoError(t, a.DoAuth("/service/message/recall", recallReq, &msg.RecallMessageRsp{}))

	// 等待 DB 写入
	time.Sleep(1 * time.Second)

	// 撤回后直查 DB：status=RECALLED(1)
	dbV.MessageStatus(t, mID, 1)

	// timeline 仍存在（不因撤回删除）
	dbV.UserTimelineExists(t, a.UserID, convID, 1)
}

// FN-DC-05 | P1 | consistency | 用户删聊天记录后直查 DB：user_timeline 删除，message 保留
func TestFN_DC_DeleteTimeline(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "will-delete-timeline")

	// 等待 MQ 消费 + DB 写入完成
	time.Sleep(1 * time.Second)

	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()

	// 删除前直查 DB：timeline 存在
	dbV.UserTimelineExists(t, a.UserID, convID, 1)

	// 删除消息（仅删当前用户的 timeline）
	delReq := &msg.DeleteMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageIds:     []int64{mID},
	}
	require.NoError(t, a.DoAuth("/service/message/delete", delReq, &msg.DeleteMessagesRsp{}))

	// 等待 DB 写入
	time.Sleep(1 * time.Second)

	// 删除后直查 DB：message 表记录保留，user_timeline 已删
	dbV.MessageExists(t, mID)
	dbV.UserTimelineExists(t, a.UserID, convID, 0)
}

// FN-DC-06 | P1 | consistency | 加好友后直查 DB：friend 表双向各 1 行
func TestFN_DC_FriendRelation(t *testing.T) {
	a, b, _ := setupConv(t) // setupConv 内部调 MakeFriends

	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()

	// 直查 DB：friend 表双向各 1 行
	dbV.FriendRelationExists(t, a.UserID, b.UserID)
	dbV.FriendRelationExists(t, b.UserID, a.UserID)
}

// FN-DC-07 | P1 | consistency | 上传后直查 DB：media_user_quota 增量正确
func TestFN_DC_MediaQuota(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	content := []byte("dc-media-quota-check")
	fileID := fixture.UploadFile(t, authed, content, "text/plain")
	require.NotEmpty(t, fileID)

	// 等待 DB 写入
	time.Sleep(1 * time.Second)

	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()

	// 上传后直查 DB：used_bytes == len(content)
	// 新用户首次上传，quota 起始为 0，上传后 used_bytes 等于文件大小
	dbV.MediaQuota(t, authed.UserID, int64(len(content)))
}
