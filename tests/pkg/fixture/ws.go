package fixture

import (
	"testing"
	"time"

	"chatnow-tests/pkg/client"
	presence "chatnow-tests/proto/chatnow/presence"
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

// ConnectWS 建立 WS 连接并完成鉴权，返回 WSClient。
// 测试结束时应调用 ws.Close() 释放连接。
func ConnectWS(t testing.TB, c *client.HTTPClient) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, c.DeviceID)
	if err != nil {
		t.Fatalf("ConnectWS: %v", err)
	}
	waitForWSOnline(t, c)
	return ws
}

// ConnectWSWithDeviceID 用指定 deviceID 建立 WS 连接。
func ConnectWSWithDeviceID(t testing.TB, c *client.HTTPClient, deviceID string) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, deviceID)
	if err != nil {
		t.Fatalf("ConnectWSWithDeviceID: %v", err)
	}
	waitForWSOnline(t, c)
	return ws
}
