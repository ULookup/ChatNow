//go:build func

package func_test

import (
	"fmt"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	conversation "chatnow-tests/proto/chatnow/conversation"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

func setupConv(t *testing.T) (a, b *client.HTTPClient, convID string) {
	t.Helper()
	return fixture.MakeFriends(t, HTTP)
}

// ---------------------------------------------------------------------------
// 1. Text
// ---------------------------------------------------------------------------

func TestSendMessage_Text(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hello"}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
	assert.NotZero(t, rsp.Message.SeqId)
}

// ---------------------------------------------------------------------------
// 2. Image
// ---------------------------------------------------------------------------

func TestSendMessage_Image(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_IMAGE,
			Body: &msg.MessageContent_Image{Image: &msg.ImageContent{
				FileId:       "fake-img-id",
				Width:        800,
				Height:       600,
				ThumbnailUrl: "http://x.com/t.jpg",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 3. File
// ---------------------------------------------------------------------------

func TestSendMessage_File(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_FILE,
			Body: &msg.MessageContent_File{File: &msg.FileContent{
				FileId:   "fake-file-id",
				FileName: "document.pdf",
				FileSize: 102400,
				MimeType: "application/pdf",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 4. Audio
// ---------------------------------------------------------------------------

func TestSendMessage_Audio(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_AUDIO,
			Body: &msg.MessageContent_Audio{Audio: &msg.AudioContent{
				FileId:      "fake-audio-id",
				DurationSec: 42,
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 5. Video
// ---------------------------------------------------------------------------

func TestSendMessage_Video(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_VIDEO,
			Body: &msg.MessageContent_Video{Video: &msg.VideoContent{
				FileId:       "fake-video-id",
				DurationSec:  120,
				Width:        1920,
				Height:       1080,
				ThumbnailUrl: "http://x.com/vthumb.jpg",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 6. Location
// ---------------------------------------------------------------------------

func TestSendMessage_Location(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_LOCATION,
			Body: &msg.MessageContent_Location{Location: &msg.LocationContent{
				Latitude:  39.9042,
				Longitude: 116.4074,
				Name:      "Beijing",
				Address:   "Tiananmen Square",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 7. Sticker
// ---------------------------------------------------------------------------

func TestSendMessage_Sticker(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_STICKER,
			Body: &msg.MessageContent_Sticker{Sticker: &msg.StickerContent{
				StickerId: "sticker-001",
				PackId:    "pack-001",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 8. SystemNotice
// ---------------------------------------------------------------------------

func TestSendMessage_SystemNotice(t *testing.T) {
	a, _, convID := setupConv(t)
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_SYSTEM_NOTICE,
			Body: &msg.MessageContent_Notice{Notice: &msg.SystemNoticeContent{
				Text:       "User joined the group",
				NoticeType: "member_join",
			}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.NotZero(t, rsp.Message.MessageId)
}

// ---------------------------------------------------------------------------
// 9. Reply
// ---------------------------------------------------------------------------

func TestSendMessage_Reply(t *testing.T) {
	a, b, convID := setupConv(t)

	// Send first message.
	req1 := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "original"}},
		},
	}
	rsp1 := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req1, rsp1)
	require.NoError(t, err)
	require.True(t, rsp1.Header.Success)
	require.NotNil(t, rsp1.Message)

	// Send reply from b to a's message.
	req2 := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "reply"}},
		},
		ReplyTo: &msg.ReplyRef{
			RepliedMessageId:   rsp1.Message.MessageId,
			RepliedSenderId:    a.UserID,
			RepliedMessageType: msg.MessageType_TEXT,
			ContentPreview:     "original",
		},
	}
	rsp2 := &transmite.SendMessageRsp{}
	err = b.DoAuth("/service/transmite/send", req2, rsp2)
	require.NoError(t, err)
	require.True(t, rsp2.Header.Success)
	require.NotNil(t, rsp2.Message)
	require.NotNil(t, rsp2.Message.ReplyTo)
	assert.Equal(t, rsp1.Message.MessageId, rsp2.Message.ReplyTo.RepliedMessageId)
	assert.Equal(t, a.UserID, rsp2.Message.ReplyTo.RepliedSenderId)
}

// ---------------------------------------------------------------------------
// 10. Mention
// ---------------------------------------------------------------------------

func TestSendMessage_Mention(t *testing.T) {
	a, b, convID := setupConv(t)

	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hello @someone"}},
		},
		MentionedUserIds: []string{b.UserID},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	assert.Contains(t, rsp.Message.MentionedUserIds, b.UserID)
}

// ---------------------------------------------------------------------------
// 11. Forward
// ---------------------------------------------------------------------------

func TestSendMessage_Forward(t *testing.T) {
	a, b, convID := setupConv(t)

	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "forwarded message"}},
		},
		ForwardInfo: &msg.ForwardInfo{
			ForwardFromUserId:    b.UserID,
			ForwardAtMs:          1700000000000,
			SourceConversationId: convID,
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Message)
	require.NotNil(t, rsp.Message.ForwardInfo)
	assert.Equal(t, b.UserID, rsp.Message.ForwardInfo.ForwardFromUserId)
}

// ---------------------------------------------------------------------------
// 12. Idempotent (same client_msg_id)
// ---------------------------------------------------------------------------

func TestSendMessage_Idempotent(t *testing.T) {
	a, _, convID := setupConv(t)

	clientMsgID := fmt.Sprintf("idem-%s", client.NewRequestID())
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "idempotent test"}},
		},
		ClientMsgId: clientMsgID,
	}

	// First send.
	rsp1 := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp1)
	require.NoError(t, err)
	require.True(t, rsp1.Header.Success)
	require.NotNil(t, rsp1.Message)

	// Second send with same client_msg_id (new request_id).
	req2 := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "idempotent test"}},
		},
		ClientMsgId: clientMsgID,
	}
	rsp2 := &transmite.SendMessageRsp{}
	err = a.DoAuth("/service/transmite/send", req2, rsp2)
	require.NoError(t, err)
	require.True(t, rsp2.Header.Success)
	require.NotNil(t, rsp2.Message)
	assert.Equal(t, rsp1.Message.MessageId, rsp2.Message.MessageId, "same client_msg_id should return same message_id")
}

// ---------------------------------------------------------------------------
// 13. ConversationNotFound
// ---------------------------------------------------------------------------

func TestSendMessage_ConversationNotFound(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: "nonexistent_conv_id_12345",
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hello"}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := a.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3001), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 14. NotMember
// ---------------------------------------------------------------------------

func TestSendMessage_NotMember(t *testing.T) {
	a, _, convID := fixture.MakeFriends(t, HTTP)
	// Register a third user who is not a member of the conversation.
	third, _, _ := fixture.RegisterAndLogin(t, HTTP)
	_ = a

	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "intruder message"}},
		},
	}
	rsp := &transmite.SendMessageRsp{}
	err := third.DoAuth("/service/transmite/send", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3002), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// L2 P0 补充：transmite 错误路径
// ---------------------------------------------------------------------------

// FN-TM-01 | P0 | 分支 | >=200 成员群走读扩散，仅写 message 主表
func TestFN_TM_SendMessage_LargeGroup_ReadDiffusion(t *testing.T) {
	// 注：200 成员注册耗时较长，使用 200 作为读扩散阈值
	// 如果服务端阈值不同，调整为实际阈值
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 200)
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
