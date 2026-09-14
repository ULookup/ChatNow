package fixture

import (
	"testing"
	"time"

	"chatnow-tests/pkg/client"
	presence "chatnow-tests/proto/chatnow/presence"
	push "chatnow-tests/proto/chatnow/push"
)

func waitForWSOnline(t testing.TB, c *client.HTTPClient) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		req := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: c.UserID}
		rsp := &presence.GetPresenceRsp{}
		if err := c.DoAuth("/service/presence/get", req, rsp); err != nil {
			t.Fatalf("wait for WS online: get presence: %v", err)
		}
		if rsp.GetHeader().GetSuccess() &&
			rsp.GetPresence().GetAggregatedState() == presence.PresenceState_ONLINE {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("wait for WS online: user %s did not become online within 10s", c.UserID)
}

// WaitDeliveryACK builds an ACK from an actual message delivered to this device.
func WaitDeliveryACK(t testing.TB, c *client.HTTPClient, ws *client.WSClient) *push.NotifyMsgPushAck {
	t.Helper()
	notify, err := ws.WaitForNotifyWithTimeout(int32(push.NotifyType_CHAT_MESSAGE_NOTIFY), 10*time.Second)
	if err != nil {
		t.Fatalf("wait for message delivery: %v", err)
	}
	message := notify.GetNewMessageInfo().GetMessageInfo()
	if message == nil || message.GetMessageId() <= 0 || message.GetUserSeq() == 0 || message.GetSeqId() == 0 {
		t.Fatal("message delivery has incomplete identity")
	}
	return &push.NotifyMsgPushAck{
		UserId: c.UserID, DeviceId: c.DeviceID,
		MessageId: message.GetMessageId(), UserSeq: message.GetUserSeq(),
		ConversationId: message.GetConversationId(), SeqId: message.GetSeqId(),
	}
}

// ConnectWS 建立 WS 连接并完成鉴权，返回 WSClient。
// 测试结束时应调用 ws.Close() 释放连接。
func ConnectWS(t testing.TB, c *client.HTTPClient) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, c.DeviceID)
	if err != nil {
		t.Fatalf("ConnectWS: %v", err)
	}
	t.Cleanup(func() { ws.Close() })
	waitForWSOnline(t, c)
	return ws
}

// ReconnectWS polls the actual upgrade endpoint after an intentional Push restart.
// Stack RPC readiness alone does not prove the WebSocket listener is accepting upgrades.
func ReconnectWS(t testing.TB, c *client.HTTPClient, timeout time.Duration) *client.WSClient {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, c.DeviceID)
		if err == nil {
			t.Cleanup(func() { _ = ws.Close() })
			waitForWSOnline(t, c)
			return ws
		}
		if time.Now().After(deadline) {
			t.Fatalf("WebSocket upgrade did not recover: %v", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// ConnectWSWithDeviceID 用指定 deviceID 建立 WS 连接。
func ConnectWSWithDeviceID(t testing.TB, c *client.HTTPClient, deviceID string) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, deviceID)
	if err != nil {
		t.Fatalf("ConnectWSWithDeviceID: %v", err)
	}
	t.Cleanup(func() { ws.Close() })
	waitForWSOnline(t, c)
	return ws
}
