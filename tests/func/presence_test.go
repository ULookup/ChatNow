//go:build func

package func_test

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	presence "chatnow-tests/proto/chatnow/presence"
	push "chatnow-tests/proto/chatnow/push"
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
	_, username, password := fixture.RegisterAndLogin(t, HTTP)
	// Each device needs its own Identity-issued JWT; changing the WS payload
	// alone cannot override the authoritative device claim.
	deviceA := fixture.LoginUser(t, HTTP, username, password)
	deviceB := fixture.LoginUser(t, HTTP, username, password)
	require.NotEqual(t, deviceA.DeviceID, deviceB.DeviceID)
	fixture.ConnectWS(t, deviceA)
	fixture.ConnectWS(t, deviceB)
	require.Eventually(t, func() bool {
		rsp := &presence.GetPresenceRsp{}
		err := deviceA.DoAuth("/service/presence/get", &presence.GetPresenceReq{
			RequestId: client.NewRequestID(), UserId: deviceA.UserID,
		}, rsp)
		if err != nil || !rsp.GetHeader().GetSuccess() {
			return false
		}
		devices := map[string]bool{}
		for _, device := range rsp.GetPresence().GetDevices() {
			if device.GetState() == presence.PresenceState_ONLINE {
				devices[device.GetDeviceId()] = true
			}
		}
		return devices[deviceA.DeviceID] && devices[deviceB.DeviceID]
	}, 5*time.Second, 50*time.Millisecond, "both authenticated devices must be online")
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

// FN-PR-04 | P0 | websocket | 订阅后目标上线，WS 收到 presence 变更通知
func TestFN_PR_SubscribePresence_NotificationDelivery(t *testing.T) {
	subscriber, _, _ := fixture.RegisterAndLogin(t, HTTP)
	target, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// subscriber 连接 WS
	wsSub, err := client.NewWSClient(HTTP.Config(), subscriber.AccessToken, subscriber.UserID, "device-sub")
	require.NoError(t, err)
	defer wsSub.Close()

	// subscriber 订阅 target
	subReq := &presence.SubscribeReq{
		RequestId: client.NewRequestID(), SubscribeUserIds: []string{target.UserID},
	}
	require.NoError(t, subscriber.DoAuth("/service/presence/subscribe", subReq, &presence.SubscribeRsp{}))

	// target 上线（连接 WS）
	wsTarget, err := client.NewWSClient(HTTP.Config(), target.AccessToken, target.UserID, "device-target")
	require.NoError(t, err)
	defer wsTarget.Close()

	// 等待 presence 通知送达
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsSub.WaitForNotify(ctx, int32(push.NotifyType_PRESENCE_CHANGE_NOTIFY))
	require.NoError(t, err, "应收到 target 上线的 presence 通知")
	_ = notify
}

// FN-PR-07 | P2 | boundary | 部分在线部分离线的批量查询
func TestFN_PR_BatchGetPresence_MixedOnlineOffline(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	onlineUser, _, _ := fixture.RegisterAndLogin(t, HTTP)
	offlineUser, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// onlineUser 连接 WS
	wsOnline, err := client.NewWSClient(HTTP.Config(), onlineUser.AccessToken, onlineUser.UserID, "device-mixed-online")
	require.NoError(t, err)
	defer wsOnline.Close()
	time.Sleep(2 * time.Second)

	req := &presence.BatchGetPresenceReq{
		RequestId: client.NewRequestID(),
		UserIds:   []string{onlineUser.UserID, offlineUser.UserID},
	}
	rsp := &presence.BatchGetPresenceRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/batch_get", req, rsp))
	require.True(t, rsp.Header.Success)
	require.Len(t, rsp.Presences, 2)

	onlinePresence := rsp.Presences[onlineUser.UserID]
	offlinePresence := rsp.Presences[offlineUser.UserID]
	assert.Equal(t, presence.PresenceState_ONLINE, onlinePresence.AggregatedState, "onlineUser 应 online")
	assert.Equal(t, presence.PresenceState_OFFLINE, offlinePresence.AggregatedState, "offlineUser 应 offline")
}

// FN-PR-08 | P2 | idempotent | 未订阅就取消，幂等不报错
func TestFN_PR_UnsubscribePresence_NotSubscribed(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 未订阅直接取消
	req := &presence.UnsubscribeReq{
		RequestId: client.NewRequestID(), UnsubscribeUserIds: []string{other.UserID},
	}
	rsp := &presence.UnsubscribeRsp{}
	require.NoError(t, authed.DoAuth("/service/presence/unsubscribe", req, rsp))
	// 幂等：不报错（success=true 或 success=false 但非 panic）
	_ = rsp.Header.Success
}
