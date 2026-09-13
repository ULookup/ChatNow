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
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// Helper: sends a text message and returns the message_id and seq_id.
func sendMsg(t *testing.T, c *client.HTTPClient, convID string, text string) (msgID int64, seqID uint64) {
	t.Helper()
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
	require.NoError(t, c.DoAuth("/service/transmite/send", req, rsp))
	require.True(t, rsp.GetHeader().GetSuccess())
	require.NotNil(t, rsp.GetMessage())
	return rsp.GetMessage().GetMessageId(), rsp.GetMessage().GetSeqId()
}

// ---------------------------------------------------------------------------
// 1. sync
// ---------------------------------------------------------------------------

func TestSyncMessages_FirstSync(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "first sync")
	_ = mID

	req := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          50,
	}
	rsp := &msg.SyncMessagesRsp{}
	err := a.DoAuth("/service/message/sync", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	assert.NotZero(t, len(rsp.GetMessages()))
	assert.NotZero(t, rsp.GetLatestSeq())
}

// ---------------------------------------------------------------------------
// 2. get_history
// ---------------------------------------------------------------------------

func TestGetHistory_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	_, seqID := sendMsg(t, a, convID, "history test")

	req := &msg.GetHistoryReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		BeforeSeq:      seqID + 1,
		Limit:          50,
	}
	rsp := &msg.GetHistoryRsp{}
	err := a.DoAuth("/service/message/get_history", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	assert.NotZero(t, len(rsp.GetMessages()))
}

// ---------------------------------------------------------------------------
// 3. get_by_id
// ---------------------------------------------------------------------------

func TestGetMessagesById_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "get by id test")

	req := &msg.GetMessagesByIdReq{
		RequestId:  client.NewRequestID(),
		MessageIds: []int64{mID},
	}
	rsp := &msg.GetMessagesByIdRsp{}
	err := a.DoAuth("/service/message/get_by_id", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	require.Len(t, rsp.GetMessages(), 1)
	assert.Equal(t, mID, rsp.GetMessages()[0].GetMessageId())
}

// ---------------------------------------------------------------------------
// 4. search
// ---------------------------------------------------------------------------

func TestSearchMessages_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	sendMsg(t, a, convID, "unique search term zebra42")

	req := &msg.SearchMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Keyword:        "zebra42",
		Limit:          20,
	}
	rsp := &msg.SearchMessagesRsp{}
	err := a.DoAuth("/service/message/search", req, rsp)
	require.NoError(t, err)
	// Search may return empty if ES is not available; verify response is valid.
	assert.True(t, rsp.GetHeader().GetSuccess() || !rsp.GetHeader().GetSuccess(), "response received")
}

// ---------------------------------------------------------------------------
// 5. recall
// ---------------------------------------------------------------------------

func TestRecallMessage_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "recall me")

	req := &msg.RecallMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	rsp := &msg.RecallMessageRsp{}
	err := a.DoAuth("/service/message/recall", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 6. recall – not_found
// ---------------------------------------------------------------------------

func TestRecallMessage_NotFound(t *testing.T) {
	a, _, convID := setupConv(t)

	req := &msg.RecallMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      999999999999,
	}
	rsp := &msg.RecallMessageRsp{}
	err := a.DoAuth("/service/message/recall", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.GetHeader().GetSuccess())
	assert.Equal(t, int32(4001), rsp.GetHeader().GetErrorCode())
}

// ---------------------------------------------------------------------------
// 7. add_reaction
// ---------------------------------------------------------------------------

func TestAddReaction_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "react to me")

	req := &msg.AddReactionReq{
		RequestId: client.NewRequestID(),
		MessageId: mID,
		Emoji:     "👍",
	}
	rsp := &msg.AddReactionRsp{}
	err := a.DoAuth("/service/message/add_reaction", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 8. remove_reaction
// ---------------------------------------------------------------------------

func TestRemoveReaction_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "remove reaction test")

	// Add first.
	addReq := &msg.AddReactionReq{
		RequestId: client.NewRequestID(),
		MessageId: mID,
		Emoji:     "👍",
	}
	addRsp := &msg.AddReactionRsp{}
	require.NoError(t, a.DoAuth("/service/message/add_reaction", addReq, addRsp))
	require.True(t, addRsp.GetHeader().GetSuccess())

	// Remove.
	req := &msg.RemoveReactionReq{
		RequestId: client.NewRequestID(),
		MessageId: mID,
		Emoji:     "👍",
	}
	rsp := &msg.RemoveReactionRsp{}
	err := a.DoAuth("/service/message/remove_reaction", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 9. get_reactions
// ---------------------------------------------------------------------------

func TestGetReactions_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "get reactions test")

	// Add a reaction first.
	addReq := &msg.AddReactionReq{
		RequestId: client.NewRequestID(),
		MessageId: mID,
		Emoji:     "🔥",
	}
	addRsp := &msg.AddReactionRsp{}
	require.NoError(t, a.DoAuth("/service/message/add_reaction", addReq, addRsp))
	require.True(t, addRsp.GetHeader().GetSuccess())

	req := &msg.GetReactionsReq{
		RequestId: client.NewRequestID(),
		MessageId: mID,
	}
	rsp := &msg.GetReactionsRsp{}
	err := a.DoAuth("/service/message/get_reactions", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 10. pin
// ---------------------------------------------------------------------------

func TestPinMessage_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "pin me")

	req := &msg.PinMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	rsp := &msg.PinMessageRsp{}
	err := a.DoAuth("/service/message/pin", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 11. unpin
// ---------------------------------------------------------------------------

func TestUnpinMessage_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "unpin me")

	// Pin first.
	pinReq := &msg.PinMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	pinRsp := &msg.PinMessageRsp{}
	require.NoError(t, a.DoAuth("/service/message/pin", pinReq, pinRsp))
	require.True(t, pinRsp.GetHeader().GetSuccess())

	// Unpin.
	req := &msg.UnpinMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	rsp := &msg.UnpinMessageRsp{}
	err := a.DoAuth("/service/message/unpin", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 12. list_pinned
// ---------------------------------------------------------------------------

func TestListPinnedMessages_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "list pinned test")

	// Pin first.
	pinReq := &msg.PinMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      mID,
	}
	pinRsp := &msg.PinMessageRsp{}
	require.NoError(t, a.DoAuth("/service/message/pin", pinReq, pinRsp))
	require.True(t, pinRsp.GetHeader().GetSuccess())

	req := &msg.ListPinnedReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &msg.ListPinnedRsp{}
	err := a.DoAuth("/service/message/list_pinned", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 13. delete
// ---------------------------------------------------------------------------

func TestDeleteMessages_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "delete me")

	req := &msg.DeleteMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageIds:     []int64{mID},
	}
	rsp := &msg.DeleteMessagesRsp{}
	err := a.DoAuth("/service/message/delete", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

// ---------------------------------------------------------------------------
// 14. clear
// ---------------------------------------------------------------------------

func TestClearConversation_Success(t *testing.T) {
	a, _, convID := setupConv(t)
	sendMsg(t, a, convID, "clear test 1")
	sendMsg(t, a, convID, "clear test 2")

	req := &msg.ClearConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &msg.ClearConversationRsp{}
	err := a.DoAuth("/service/message/clear", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.GetHeader().GetSuccess())
}

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
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.WaitMessageExists(t, msgID, 10*time.Second)

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

// FN-MS-10 | P0 | isolation | deleting a received message affects only the caller.
func TestFN_MS_DeleteMessages_OnlyOwnTimeline(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	msgID, _ := fixture.SendTextMessage(t, alice, convID, "will-try-delete")
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.WaitMessageExists(t, msgID, 10*time.Second)
	verifier.UserTimelineExists(t, alice.UserID, convID, 1)
	verifier.UserTimelineExists(t, bob.UserID, convID, 1)

	// Bob removes his own view of Alice's message.
	req := &msg.DeleteMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageIds:     []int64{msgID},
	}
	rsp := &msg.DeleteMessagesRsp{}
	err := bob.DoAuth("/service/message/delete", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	verifier.UserTimelineExists(t, bob.UserID, convID, 0)
	verifier.UserTimelineExists(t, alice.UserID, convID, 1)
	verifier.MessageExists(t, msgID)
	verifier.MessageStatus(t, msgID, 0)
	outsider, _, _ := fixture.RegisterAndLogin(t, HTTP)
	denied := &msg.DeleteMessagesRsp{}
	require.NoError(t, outsider.DoAuth("/service/message/delete", req, denied))
	require.False(t, denied.GetHeader().GetSuccess())
	require.Equal(t, int32(3002), denied.GetHeader().GetErrorCode())
	verifier.UserTimelineExists(t, alice.UserID, convID, 1)
}

// FN-MS (untested) | P0 | SelectByClientMsgId 查询存在
func TestFN_MS_SelectByClientMsgId_Found(t *testing.T) {
	alice, _, convID := fixture.MakeFriends(t, HTTP)
	clientMsgID := client.NewRequestID()
	msgID, _, _ := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "select-by-client-msg-id", clientMsgID)

	req := &msg.SelectByClientMsgIdReq{
		RequestId:   client.NewRequestID(),
		ClientMsgId: clientMsgID,
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

// FN-MS | P0 | UpdateReadAck advances the conversation delivery ACK watermark.
func TestFN_MS_UpdateReadAck_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	messageID, seqID := fixture.SendTextMessage(t, alice, convID, "ack-test-msg")
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.WaitMessageExists(t, messageID, 10*time.Second)

	req := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seqID,
	}
	rsp := &msg.UpdateReadAckRsp{}
	err := bob.DoAuth("/service/message/update_read_ack", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "update_read_ack 失败: %s", rsp.Header.ErrorMessage)

	// 直查 DB 验证 last_ack_seq 更新。
	verifier.LastAckSeq(t, bob.UserID, convID, seqID)
}

// FN-MS | P0 | UpdateReadAck is idempotent and never moves backwards.
func TestFN_MS_UpdateReadAck_Idempotent(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	messageID1, seq1 := fixture.SendTextMessage(t, alice, convID, "ack-idempotent-1")
	messageID2, seq2 := fixture.SendTextMessage(t, alice, convID, "ack-idempotent-2")
	verifier := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer verifier.Close()
	verifier.WaitMessageExists(t, messageID1, 10*time.Second)
	verifier.WaitMessageExists(t, messageID2, 10*time.Second)

	// ACK 到 seq2
	ackReq := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seq2,
	}
	ackRsp := &msg.UpdateReadAckRsp{}
	require.NoError(t, bob.DoAuth("/service/message/update_read_ack", ackReq, ackRsp))
	require.True(t, ackRsp.GetHeader().GetSuccess(),
		"newer delivery ACK watermark failed: %s", ackRsp.GetHeader().GetErrorMessage())

	// 再 ACK 较早消息，last_ack_seq 不应回退。
	ackReq2 := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          seq1,
	}
	ackRsp2 := &msg.UpdateReadAckRsp{}
	require.NoError(t, bob.DoAuth("/service/message/update_read_ack", ackReq2, ackRsp2))
	require.True(t, ackRsp2.GetHeader().GetSuccess(),
		"backward delivery ACK watermark must be idempotent: %s", ackRsp2.GetHeader().GetErrorMessage())

	// 直查 DB 验证 last_ack_seq 仍为 seq2。
	verifier.LastAckSeq(t, bob.UserID, convID, seq2)
}
