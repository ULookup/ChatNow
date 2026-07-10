package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
)

// ConnectWS 建立 WS 连接并完成鉴权，返回 WSClient。
// 测试结束时应调用 ws.Close() 释放连接。
func ConnectWS(t testing.TB, c *client.HTTPClient) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, c.DeviceID)
	if err != nil {
		t.Fatalf("ConnectWS: %v", err)
	}
	return ws
}

// ConnectWSWithDeviceID 用指定 deviceID 建立 WS 连接。
func ConnectWSWithDeviceID(t testing.TB, c *client.HTTPClient, deviceID string) *client.WSClient {
	ws, err := client.NewWSClient(c.Config(), c.AccessToken, c.UserID, deviceID)
	if err != nil {
		t.Fatalf("ConnectWSWithDeviceID: %v", err)
	}
	return ws
}
