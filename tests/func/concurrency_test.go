//go:build func

package func_test

import (
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-CC-01 | P0 | 并发 | 10 goroutine 用相同 client_msg_id 发消息，仅 1 条落库
func TestFN_CC_SendMessage_SameClientMsgId(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	clientMsgID := client.NewRequestID()

	// 10 goroutine 并发用相同 client_msg_id 发消息
	var wg sync.WaitGroup
	successCount := 0
	var mu sync.Mutex
	results := make([]int64, 0, 10)

	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: convID,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "concurrent-same-id"}},
				},
				ClientMsgId: clientMsgID,
			}
			rsp := &transmite.SendMessageRsp{}
			err := alice.DoAuth("/service/transmite/send", req, rsp)
			if err == nil && rsp.Header.Success && rsp.Message != nil {
				mu.Lock()
				successCount++
				results = append(results, rsp.Message.MessageId)
				mu.Unlock()
			}
		}()
	}
	wg.Wait()

	// 至少 1 次成功
	require.GreaterOrEqual(t, successCount, 1, "至少 1 次发送应成功")

	// 所有成功的请求应返回相同 message_id（幂等）
	firstID := results[0]
	for _, id := range results {
		assert.Equal(t, firstID, id, "相同 client_msg_id 应返回相同 message_id")
	}

	// 直查 DB：仅有 1 条消息
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}
