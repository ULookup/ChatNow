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
