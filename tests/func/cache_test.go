//go:build func

package func_test

import (
	"fmt"
	"testing"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-CA-05 | healthy Redis applies the distributed message rate limit.
func TestFN_CA_RateLimit(t *testing.T) {
	user, peer, convID := fixture.MakeFriends(t, HTTP)
	_ = peer

	rateLimited := 0
	for i := 0; i < 650; i++ {
		rsp := &transmite.SendMessageRsp{}
		err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
			RequestId:      client.NewRequestID(),
			ConversationId: convID,
			Content: &msg.MessageContent{
				Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("rate-limit-%d", i)}},
			},
			ClientMsgId: client.NewRequestID(),
		}, rsp)
		require.NoError(t, err)
		if rsp.GetHeader().GetErrorMessage() == "rate_limited" {
			rateLimited++
		}
	}

	require.Greater(t, rateLimited, 0, "healthy Redis must enforce the distributed rate limit")
}
