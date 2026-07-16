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
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

// FN-WS-01 | P0 | WebSocket 推送 | 发消息后接收方 WS 收到 CHAT_MESSAGE_NOTIFY
func TestFN_WS_NewMessageNotify(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// bob 建立 WS 连接
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

	// 等待 WS 鉴权完成
	time.Sleep(500 * time.Millisecond)

	// alice 发消息
	fixture.SendTextMessage(t, alice, convID, "ws-notify-test")

	// bob WS 应收到 CHAT_MESSAGE_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err, "10s 内未收到 CHAT_MESSAGE_NOTIFY")
	assert.NotNil(t, notify.GetNewMessageInfo(), "通知应包含 NewMessageInfo")

	// 验证消息内容
	actualMsg := notify.GetNewMessageInfo().GetMessageInfo()
	if actualMsg != nil {
		assert.Equal(t, "ws-notify-test", actualMsg.GetContent().GetText().Text)
	}
	_ = msg.MessageType_TEXT
}

// FN-WS-02 | P0 | WebSocket 推送 | 好友申请后被申请方 WS 收到 FRIEND_ADD_APPLY_NOTIFY
func TestFN_WS_FriendRequestNotify(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// bob 建立 WS 连接
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

	// 等待 WS 鉴权完成
	time.Sleep(500 * time.Millisecond)

	// alice 向 bob 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bob.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// bob WS 应收到 FRIEND_ADD_APPLY_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_FRIEND_ADD_APPLY_NOTIFY))
	require.NoError(t, err, "10s 内未收到 FRIEND_ADD_APPLY_NOTIFY")
	assert.NotNil(t, notify.GetFriendAddApply(), "通知应包含 FriendAddApply")
	// 验证申请人信息
	applyInfo := notify.GetFriendAddApply().GetUserInfo()
	if applyInfo != nil {
		assert.Equal(t, alice.UserID, applyInfo.UserId)
	}
}
