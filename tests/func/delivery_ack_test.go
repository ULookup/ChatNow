//go:build func

package func_test

import (
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	conversation "chatnow-tests/proto/chatnow/conversation"
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
)

// FN-MS-02 | P0 | Membership exit and delivery convergence share one transaction boundary.
func TestFN_MS_DeliveryAckMembershipRace(t *testing.T) {
	sender, recipient, convID := fixture.MakeFriends(t, HTTP)
	_, seq := fixture.SendTextMessage(t, sender, convID, "delivery-membership-race")
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	t.Cleanup(db.Close)
	commitExit, blocked := db.HoldMemberExit(t, recipient.UserID, convID)
	rsp := &msg.UpdateReadAckRsp{}
	result := make(chan error, 1)
	go func() {
		result <- recipient.DoAuth("/service/message/update_read_ack", &msg.UpdateReadAckReq{
			RequestId: client.NewRequestID(), ConversationId: convID, SeqId: seq,
		}, rsp)
	}()
	require.Eventually(t, blocked, 2*time.Second, 10*time.Millisecond, "ACK must reach the held member row")
	commitExit()
	select {
	case err := <-result:
		require.NoError(t, err)
	case <-time.After(5 * time.Second):
		t.Fatal("delivery ACK did not complete after membership exit")
	}
	require.False(t, rsp.GetHeader().GetSuccess(), "a member who left before convergence cannot advance the cursor")
	require.EqualValues(t, 3002, rsp.GetHeader().GetErrorCode())
	db.LastAckSeq(t, recipient.UserID, convID, 0)
}

// FN-WS-11 | P0 | Optional watermarks, duplicate ACKs, and out-of-order ACKs preserve semantics.
func TestFN_WS_DeliveryAckCompatibility(t *testing.T) {
	sender, recipient, convID := fixture.MakeFriends(t, HTTP)
	ws := fixture.ConnectWS(t, recipient)
	fixture.SendTextMessage(t, sender, convID, "delivery-first")
	first := fixture.WaitDeliveryACK(t, recipient, ws)
	fixture.SendTextMessage(t, sender, convID, "delivery-second")
	second := fixture.WaitDeliveryACK(t, recipient, ws)
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	t.Cleanup(db.Close)
	key := "im:unack:idx:{" + recipient.UserID + ":" + recipient.DeviceID + "}"
	for _, original := range []*push.NotifyMsgPushAck{second, first, second} {
		ack := proto.Clone(original).(*push.NotifyMsgPushAck)
		ack.ConversationId, ack.SeqId = "", 0
		require.NoError(t, ws.SendNotify(&push.NotifyMessage{
			NotifyType:    push.NotifyType_MSG_PUSH_ACK,
			NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: ack},
		}))
		require.Eventually(t, func() bool {
			return verify.RedisCLI(t, "HEXISTS", key, strconv.FormatUint(ack.UserSeq, 10)) == "0" &&
				db.ReadLastAckSeq(t, recipient.UserID, convID) == second.SeqId
		}, 5*time.Second, 20*time.Millisecond)
	}
	db.LastReadSeq(t, recipient.UserID, convID, 0)
}

// FN-WS-12 | P0 | A failed Message response retains the pending delivery.
func TestFN_WS_DeliveryAckBusinessFailure(t *testing.T) {
	sender, recipient, _ := fixture.MakeFriends(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, sender, []*client.HTTPClient{recipient}, "ack-business-failure")
	ws := fixture.ConnectWS(t, recipient)
	fixture.SendTextMessage(t, sender, convID, "delivery-before-exit")
	ack := fixture.WaitDeliveryACK(t, recipient, ws)
	rsp := &conversation.QuitConversationRsp{}
	require.NoError(t, recipient.DoAuth("/service/conversation/quit", &conversation.QuitConversationReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
	}, rsp))
	require.True(t, rsp.GetHeader().GetSuccess())
	key := "im:unack:idx:{" + recipient.UserID + ":" + recipient.DeviceID + "}"
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	t.Cleanup(db.Close)
	before := verify.BVar(t, HTTP.Config().Infra.PushVars, "push_delivery_ack_failure_total")
	require.NoError(t, ws.SendNotify(&push.NotifyMessage{
		NotifyType:    push.NotifyType_MSG_PUSH_ACK,
		NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: ack},
	}))
	require.Eventually(t, func() bool {
		return verify.BVar(t, HTTP.Config().Infra.PushVars, "push_delivery_ack_failure_total") > before
	}, 5*time.Second, 20*time.Millisecond, "observe the failed Message response before checking retained state")
	require.Equal(t, "1", verify.RedisCLI(t, "HEXISTS", key, strconv.FormatUint(ack.UserSeq, 10)))
	db.LastAckSeq(t, recipient.UserID, convID, 0)
}

// FN-WS-10 | P0 | Forged delivery ACK fields cannot delete state or advance a cursor.
func TestFN_WS_DeliveryAckRejectsForgery(t *testing.T) {
	for _, field := range []string{"seq_id", "message_id", "conversation_id", "user_seq", "user_id", "device_id"} {
		t.Run(field, func(t *testing.T) {
			sender, recipient, convID := fixture.MakeFriends(t, HTTP)
			ws := fixture.ConnectWS(t, recipient)
			fixture.SendTextMessage(t, sender, convID, "delivery-ack-integrity")
			ack := fixture.WaitDeliveryACK(t, recipient, ws)
			forged := proto.Clone(ack).(*push.NotifyMsgPushAck)
			switch field {
			case "seq_id":
				forged.SeqId += 1000
			case "message_id":
				forged.MessageId++
			case "conversation_id":
				forged.ConversationId = client.NewRequestID()
			case "user_seq":
				forged.UserSeq++
			case "user_id":
				forged.UserId = sender.UserID
			case "device_id":
				forged.DeviceId = client.NewDeviceID()
			}
			key := "im:unack:idx:{" + recipient.UserID + ":" + recipient.DeviceID + "}"
			member := strconv.FormatUint(ack.UserSeq, 10)
			require.Equal(t, "1", verify.RedisCLI(t, "HEXISTS", key, member))
			db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
			t.Cleanup(db.Close)
			require.NoError(t, ws.SendNotify(&push.NotifyMessage{
				NotifyType:    push.NotifyType_MSG_PUSH_ACK,
				NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: forged},
			}))
			// A negative observation window under an established invalid input;
			// successful convergence below polls actual Redis and MySQL state.
			require.Never(t, func() bool {
				return verify.RedisCLI(t, "HEXISTS", key, member) != "1" ||
					db.ReadLastAckSeq(t, recipient.UserID, convID) != 0
			}, 500*time.Millisecond, 20*time.Millisecond, "forged ACK changed authoritative delivery state")
			require.NoError(t, ws.SendNotify(&push.NotifyMessage{
				NotifyType:    push.NotifyType_MSG_PUSH_ACK,
				NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: ack},
			}))
			require.Eventually(t, func() bool {
				return verify.RedisCLI(t, "HEXISTS", key, member) == "0" &&
					db.ReadLastAckSeq(t, recipient.UserID, convID) == ack.SeqId
			}, 5*time.Second, 20*time.Millisecond, "valid ACK must converge before removal")
			db.LastReadSeq(t, recipient.UserID, convID, 0)
		})
	}
}
