//go:build perf

package perf_test

import (
	"fmt"
	"testing"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

func BenchmarkSyncMessages(b *testing.B) {
	a, bb, convID := fixture.MakeFriends(b, HTTP)
	_ = bb

	// Pre-populate with messages
	for i := 0; i < 50; i++ {
		req := &transmite.SendMessageReq{
			RequestId: client.NewRequestID(), ConversationId: convID,
			Content: &msg.MessageContent{
				Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("sync perf %d", i)}},
			},
			ClientMsgId: client.NewRequestID(),
		}
		rsp := &transmite.SendMessageRsp{}
		if err := a.DoAuth("/service/transmite/send", req, rsp); err != nil {
			b.Fatal(err)
		}
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		syncReq := &msg.SyncMessagesReq{
			RequestId: client.NewRequestID(), ConversationId: convID,
			AfterSeq: 0, Limit: 20,
		}
		syncRsp := &msg.SyncMessagesRsp{}
		if err := a.DoAuth("/service/message/sync", syncReq, syncRsp); err != nil {
			b.Fatal(err)
		}
		if !syncRsp.Header.Success {
			b.Fatal("sync failed")
		}
	}
}
