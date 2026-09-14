//go:build reliability

package reliability_test

import (
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	push "chatnow-tests/proto/chatnow/push"
)

// RL-ACK-01 | P0 | Failed delivery convergence retains exact per-device retry state.
func TestRL_DeliveryAckRetainedDuringMessageOutage(t *testing.T) {
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	t.Cleanup(func() { cleanup.CleanupAll(t, HTTP.Config()) })
	sender, recipient, convID := fixture.MakeFriends(t, HTTP)
	ws := fixture.ConnectWS(t, recipient)
	fixture.SendTextMessage(t, sender, convID, "ack-during-message-outage")
	ack := fixture.WaitDeliveryACK(t, recipient, ws)
	key := "im:unack:idx:{" + recipient.UserID + ":" + recipient.DeviceID + "}"
	member := strconv.FormatUint(ack.UserSeq, 10)
	require.Equal(t, "1", verify.RedisCLI(t, "HEXISTS", key, member))
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	t.Cleanup(db.Close)
	restore := chaos.StopMessage(t, HTTP.Config())
	t.Cleanup(func() {
		restore()
		require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	})
	frame := &push.NotifyMessage{
		NotifyType:    push.NotifyType_MSG_PUSH_ACK,
		NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: ack},
	}
	require.NoError(t, ws.SendNotify(frame))
	require.Never(t, func() bool {
		return verify.RedisCLI(t, "HEXISTS", key, member) != "1" ||
			db.ReadLastAckSeq(t, recipient.UserID, convID) != 0
	}, 2*time.Second, 50*time.Millisecond, "failed Message RPC must retain retry state")
	restartPush := chaos.StopPush(t, HTTP.Config())
	restartPush()
	restore()
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	ws = fixture.ReconnectWS(t, recipient, 15*time.Second)
	require.NoError(t, ws.SendNotify(&push.NotifyMessage{
		NotifyType: push.NotifyType_CLIENT_HEARTBEAT,
		NotifyRemarks: &push.NotifyMessage_Heartbeat{Heartbeat: &push.NotifyHeartbeat{
			UserId: recipient.UserID, LastUserSeq: ack.UserSeq,
		}},
	}))
	replayed := fixture.WaitDeliveryACK(t, recipient, ws)
	require.Equal(t, ack.MessageId, replayed.MessageId)
	require.Equal(t, ack.UserSeq, replayed.UserSeq)
	// Retrying the original device ACK is idempotent across uncertain RPC outcomes.
	require.Eventually(t, func() bool {
		if err := ws.SendNotify(frame); err != nil {
			t.Fatalf("send recovery ACK: %v", err)
		}
		return verify.RedisCLI(t, "HEXISTS", key, member) == "0" &&
			db.ReadLastAckSeq(t, recipient.UserID, convID) == ack.SeqId
	}, 10*time.Second, 100*time.Millisecond)
	db.LastReadSeq(t, recipient.UserID, convID, 0)
}
