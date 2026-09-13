//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-TM-02 | P0 | Existing accepted records cannot produce partial success.
func TestFN_TM_IdempotentRecordWithoutReadableMessage(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	db := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer db.Close()
	for _, value := range []string{"accepted:42", "persisted:42"} {
		t.Run(value, func(t *testing.T) {
			clientID := client.NewRequestID()
			key := "im:msg:idem:" + user.UserID + ":" + clientID
			t.Cleanup(func() { verify.RedisCLI(t, "DEL", key) })
			verify.RedisCLI(t, "SET", key, value, "EX", "86400")
			rsp := &transmite.SendMessageRsp{}
			require.NoError(t, user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
				RequestId: client.NewRequestID(), ConversationId: convID, ClientMsgId: clientID,
				Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "unreadable-original"}}},
			}, rsp))
			require.False(t, rsp.GetHeader().GetSuccess(), "an unreadable original must not become partial success")
			require.EqualValues(t, 9002, rsp.GetHeader().GetErrorCode())
			require.Nil(t, rsp.GetMessage())
			require.Equal(t, value, verify.RedisCLI(t, "GET", key), "do not remove a prior acceptance guard")
			db.MessageCount(t, convID, 0)
		})
	}
}
