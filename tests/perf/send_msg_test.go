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

func BenchmarkSendMessage(b *testing.B) {
	const numConvs = 20
	type conv struct {
		sender *client.HTTPClient
		id     string
	}
	convs := make([]conv, numConvs)
	for i := 0; i < numConvs; i++ {
		a, bb, convID := fixture.MakeFriends(b, HTTP)
		convs[i] = conv{sender: a, id: convID}
		_ = bb
	}

	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		i := 0
		for pb.Next() {
			c := convs[i%numConvs]
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: c.id,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("perf msg %d", i)}},
				},
				ClientMsgId: client.NewRequestID(),
			}
			rsp := &transmite.SendMessageRsp{}
			if err := c.sender.DoAuth("/service/transmite/send", req, rsp); err != nil {
				b.Fatal(err)
			}
			if !rsp.Header.Success {
				b.Fatalf("send failed: %s", rsp.Header.ErrorMessage)
			}
			i++
		}
	})
}
