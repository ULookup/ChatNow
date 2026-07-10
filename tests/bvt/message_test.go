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
	assert.Equal(t, "sync test msg", rsp.Messages[0].GetContent().GetText().Text)
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
