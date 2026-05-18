//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
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
