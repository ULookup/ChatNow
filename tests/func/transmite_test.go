//go:build func

package func_test

import (
	"fmt"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
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
