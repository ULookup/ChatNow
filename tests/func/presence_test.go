//go:build func

package func_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	presence "chatnow-tests/proto/chatnow/presence"
)

func TestGetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: other.UserID}
	rsp := &presence.GetPresenceRsp{}
	err := authed.DoAuth("/service/presence/get", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestBatchGetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.BatchGetPresenceReq{
		RequestId: client.NewRequestID(), UserIds: []string{authed.UserID, other.UserID},
	}
	rsp := &presence.BatchGetPresenceRsp{}
	err := authed.DoAuth("/service/presence/batch_get", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSubscribePresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.SubscribeReq{
		RequestId: client.NewRequestID(), SubscribeUserIds: []string{other.UserID},
	}
	rsp := &presence.SubscribeRsp{}
	err := authed.DoAuth("/service/presence/subscribe", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestUnsubscribePresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	subReq := &presence.SubscribeReq{RequestId: client.NewRequestID(), SubscribeUserIds: []string{other.UserID}}
	require.NoError(t, authed.DoAuth("/service/presence/subscribe", subReq, &presence.SubscribeRsp{}))
	req := &presence.UnsubscribeReq{
		RequestId: client.NewRequestID(), UnsubscribeUserIds: []string{other.UserID},
	}
	rsp := &presence.UnsubscribeRsp{}
	err := authed.DoAuth("/service/presence/unsubscribe", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSendTyping_PrivateChat_True(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	cid := "p_" + a.UserID + "_" + b.UserID
	if a.UserID > b.UserID {
		cid = "p_" + b.UserID + "_" + a.UserID
	}

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: cid,
		IsTyping:       true,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSendTyping_PrivateChat_False(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	cid := "p_" + a.UserID + "_" + b.UserID
	if a.UserID > b.UserID {
		cid = "p_" + b.UserID + "_" + a.UserID
	}

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: cid,
		IsTyping:       false,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSendTyping_GroupChat_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: "g_some_group_id",
		IsTyping:       true,
	}
	rsp := &presence.TypingRsp{}
	err := a.DoAuth("/service/presence/send_typing", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// FN-PR-01 | P1 | state transition | 同用户多设备在线，presence 为 online
func TestFN_PR_GetPresence_MultiDevice(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 设备 A 连接 WS
	wsA, err := client.NewWSClient(HTTP.Config(), authed.AccessToken, authed.UserID, "device-A")
	require.NoError(t, err)
	defer wsA.Close()

	// 设备 B 连接 WS（同用户不同设备）
	wsB, err := client.NewWSClient(HTTP.Config(), authed.AccessToken, authed.UserID, "device-B")
	require.NoError(t, err)
	defer wsB.Close()

	// 查询 presence，应为 ONLINE
	req := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: authed.UserID}
	rsp := &presence.GetPresenceRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/get", req, rsp))
	require.True(t, rsp.Header.Success)
	assert.Equal(t, presence.PresenceState_ONLINE, rsp.Presence.AggregatedState)
	assert.GreaterOrEqual(t, len(rsp.Presence.Devices), 2, "多设备应列出 >=2 个 device")
}

// FN-PR-02 | P1 | state transition | 心跳续期，TTL 刷新
func TestFN_PR_Presence_HeartbeatRefresh(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	ws, err := client.NewWSClient(HTTP.Config(), authed.AccessToken, authed.UserID, "device-heartbeat")
	require.NoError(t, err)
	defer ws.Close()

	// 等 2s 让 presence 记录上线
	time.Sleep(2 * time.Second)

	// 查询 presence 确认 online
	req1 := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: authed.UserID}
	rsp1 := &presence.GetPresenceRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/get", req1, rsp1))
	require.True(t, rsp1.Header.Success)
	assert.Equal(t, presence.PresenceState_ONLINE, rsp1.Presence.AggregatedState)

	// 等 3s（心跳应自动续期）
	time.Sleep(3 * time.Second)

	// 再次查询，仍应 online（心跳续期生效）
	req2 := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: authed.UserID}
	rsp2 := &presence.GetPresenceRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/get", req2, rsp2))
	require.True(t, rsp2.Header.Success)
	assert.Equal(t, presence.PresenceState_ONLINE, rsp2.Presence.AggregatedState, "心跳续期后应仍 online")
}

// FN-PR-03 | P1 | state transition | WS 断开后 presence 变 offline
func TestFN_PR_Presence_OfflineOnDisconnect(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	ws, err := client.NewWSClient(HTTP.Config(), authed.AccessToken, authed.UserID, "device-offline")
	require.NoError(t, err)

	// 等待上线
	time.Sleep(2 * time.Second)
	ws.Close()

	// 等待服务端检测断开 + TTL 过期
	time.Sleep(5 * time.Second)

	req := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: authed.UserID}
	rsp := &presence.GetPresenceRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/get", req, rsp))
	require.True(t, rsp.Header.Success)
	// 断开后应 offline（或无在线设备）
	assert.Equal(t, presence.PresenceState_OFFLINE, rsp.Presence.AggregatedState,
		"WS 断开后 presence 应变 offline")
}
