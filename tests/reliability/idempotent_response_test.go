//go:build reliability

package reliability_test

import (
	"fmt"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	authmeta "chatnow-tests/proto/chatnow/common/auth"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// RL-MESSAGE-01 | P0 | Idempotent success stays complete across lookup outages.
func TestRL_IdempotentResponseDuringMessageOutage(t *testing.T) {
	for _, persisted := range []bool{false, true} {
		t.Run(fmt.Sprintf("persisted_%t", persisted), func(t *testing.T) {
			require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
			// This layer owns a disposable synthetic stack; restore the service
			// before suite cleanup touches any asynchronous message state.
			t.Cleanup(func() { cleanup.CleanupAll(t, HTTP.Config()) })
			user, peer, convID := fixture.MakeFriends(t, HTTP)
			db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
			t.Cleanup(db.Close)
			clientID := client.NewRequestID()
			endpoints := reliabilityTransmiteEndpoints(t)
			require.Len(t, endpoints, 1)
			metadata, err := proto.Marshal(&authmeta.RpcMetadata{
				TraceId: client.NewRequestID(), UserId: user.UserID, DeviceId: user.DeviceID,
			})
			require.NoError(t, err)
			send := func() *transmite.SendMessageRsp {
				rsp := &transmite.SendMessageRsp{}
				require.NoError(t, user.DoInternalRPC(endpoints[0], "chatnow.transmite.MsgTransmitService", "SendMessage", &transmite.SendMessageReq{
					RequestId: client.NewRequestID(), ConversationId: convID, ClientMsgId: clientID,
					Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
						Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "idempotent-outage"}}},
				}, rsp, metadata))
				return rsp
			}
			var restore func()
			if !persisted {
				restore = chaos.StopMessage(t, HTTP.Config())
			}
			first := send()
			require.True(t, first.GetHeader().GetSuccess())
			require.NotZero(t, first.GetMessage().GetMessageId())
			require.NotZero(t, first.GetMessage().GetSeqId())
			if persisted {
				db.WaitMessageExists(t, first.GetMessage().GetMessageId(), 10*time.Second)
				require.True(t, send().GetHeader().GetSuccess(), "direct duplicate control must succeed before the fault")
				restore = chaos.StopMessage(t, HTTP.Config())
			} else {
				db.MessageCount(t, convID, 0)
			}
			t.Cleanup(func() {
				restore()
				db.WaitMessageExists(t, first.GetMessage().GetMessageId(), 10*time.Second)
				require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second), "fault cleanup must restore service readiness")
			})
			key := "im:msg:idem:" + user.UserID + ":" + clientID
			for _, state := range []string{"accepted", "persisted"} {
				// Exercise both existing cache formats, without changing the
				// identity of the broker-confirmed message or replaying publication.
				value := fmt.Sprintf("%s:%d", state, first.GetMessage().GetMessageId())
				verify.RedisCLI(t, "SET", key, value, "EX", "86400")
				started := time.Now()
				repeated := send()
				if repeated.GetHeader().GetSuccess() {
					require.Equal(t, first.GetMessage().GetMessageId(), repeated.GetMessage().GetMessageId())
					require.Equal(t, first.GetMessage().GetSeqId(), repeated.GetMessage().GetSeqId(),
						"successful duplicate must preserve the original nonzero sequence")
					require.Equal(t, convID, repeated.GetMessage().GetConversationId())
				} else {
					require.EqualValues(t, 9002, repeated.GetHeader().GetErrorCode())
					require.Nil(t, repeated.GetMessage(), "unavailable lookup must not expose a partial result")
				}
				require.Less(t, time.Since(started), 5*time.Second, "lookup must be bounded")
				require.Equal(t, value, verify.RedisCLI(t, "GET", key), "retain the deduplication guard")
			}
			restore()
			db.WaitMessageExists(t, first.GetMessage().GetMessageId(), 10*time.Second)
			for _, state := range []string{"accepted", "persisted", "pending"} {
				value := state
				if state != "pending" {
					value = fmt.Sprintf("%s:%d", state, first.GetMessage().GetMessageId())
				}
				verify.RedisCLI(t, "SET", key, value, "EX", "86400")
				deadline := time.Now().Add(10 * time.Second)
				recovered := send()
				for !recovered.GetHeader().GetSuccess() && time.Now().Before(deadline) {
					require.EqualValues(t, 9002, recovered.GetHeader().GetErrorCode())
					require.Nil(t, recovered.GetMessage())
					time.Sleep(50 * time.Millisecond)
					recovered = send()
				}
				if !recovered.GetHeader().GetSuccess() {
					lookup := &msg.SelectByClientMsgIdRsp{}
					err := user.DoAuth("/service/message/select_by_client_msg_id", &msg.SelectByClientMsgIdReq{
						RequestId: client.NewRequestID(), ClientMsgId: clientID,
					}, lookup)
					t.Logf("recovery diagnostic: duplicate_code=%d lookup_transport_ok=%t lookup_success=%t lookup_code=%d has_message=%t nonzero_seq=%t",
						recovered.GetHeader().GetErrorCode(), err == nil, lookup.GetHeader().GetSuccess(), lookup.GetHeader().GetErrorCode(),
						lookup.GetMessage() != nil, lookup.GetMessage().GetSeqId() != 0)
				}
				require.True(t, recovered.GetHeader().GetSuccess(), "recovery must return the original message")
				require.Equal(t, first.GetMessage().GetMessageId(), recovered.GetMessage().GetMessageId())
				require.Equal(t, first.GetMessage().GetSeqId(), recovered.GetMessage().GetSeqId())
				require.Equal(t, convID, recovered.GetMessage().GetConversationId())
				require.Equal(t, "idempotent-outage", recovered.GetMessage().GetContent().GetText().GetText())
			}
			db.MessageCount(t, convID, 1)
			db.UserTimelineExists(t, user.UserID, convID, 1)
			db.UserTimelineExists(t, peer.UserID, convID, 1)
		})
	}
}
