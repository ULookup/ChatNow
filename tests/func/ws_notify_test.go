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
	presence "chatnow-tests/proto/chatnow/presence"
	push "chatnow-tests/proto/chatnow/push"
	relationship "chatnow-tests/proto/chatnow/relationship"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-WS-01 | P0 | WebSocket 推送 | 发消息后接收方 WS 收到 CHAT_MESSAGE_NOTIFY
func TestFN_WS_NewMessageNotify(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// bob 建立 WS 连接
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

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

// FN-WS-03 | P1 | websocket | 好友申请通过后，申请方 WS 收到通知
func TestFN_WS_FriendAcceptNotify(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// a 连接 WS
	wsA := fixture.ConnectWSWithDeviceID(t, a, "device-ws03")
	defer wsA.Close()

	// a 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: b.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// b 通过申请
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq, &relationship.HandleFriendRsp{}))

	// a 应收到 FRIEND_ADD_PROCESS_NOTIFY（好友申请被处理）
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := wsA.WaitForNotify(ctx, int32(push.NotifyType_FRIEND_ADD_PROCESS_NOTIFY))
	require.NoError(t, err, "a 应收到好友通过通知")
}

// FN-WS-04 | P1 | websocket | 会话创建后，成员 WS 收到通知
func TestFN_WS_ConversationCreateNotify(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// member 连接 WS
	wsMember := fixture.ConnectWSWithDeviceID(t, member, "device-ws04")
	defer wsMember.Close()

	// owner 建群（含 member）
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "ws-conv-create-test")

	// member 应收到 CONVERSATION_CREATE_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := wsMember.WaitForNotify(ctx, int32(push.NotifyType_CONVERSATION_CREATE_NOTIFY))
	require.NoError(t, err, "member 应收到会话创建通知")
	_ = convID
}

// FN-WS-05 | P1 | websocket | 订阅的用户上线/离线，WS 收到通知
func TestFN_WS_PresenceChangeNotify(t *testing.T) {
	subscriber, _, _ := fixture.RegisterAndLogin(t, HTTP)
	target, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// subscriber 连接 WS
	wsSub := fixture.ConnectWSWithDeviceID(t, subscriber, "device-ws05-sub")
	defer wsSub.Close()

	// subscriber 订阅 target
	subReq := &presence.SubscribeReq{
		RequestId:        client.NewRequestID(),
		SubscribeUserIds: []string{target.UserID},
	}
	require.NoError(t, subscriber.DoAuth("/service/presence/subscribe", subReq, &presence.SubscribeRsp{}))

	// target 上线
	wsTarget := fixture.ConnectWSWithDeviceID(t, target, "device-ws05-target")
	defer wsTarget.Close()

	// subscriber 应收到 PRESENCE_CHANGE_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := wsSub.WaitForNotify(ctx, int32(push.NotifyType_PRESENCE_CHANGE_NOTIFY))
	require.NoError(t, err, "subscriber 应收到 target 上线通知")
}

// FN-WS-06 | P1 | websocket | WS 断开后重连，遗漏消息通过 sync 补齐
func TestFN_WS_Reconnect(t *testing.T) {
	a, b, convID := setupConv(t)

	// b 连接 WS
	wsB1 := fixture.ConnectWSWithDeviceID(t, b, "device-ws06-1")

	// b 断开 WS
	require.NoError(t, wsB1.Close())

	// a 发消息（b 离线）
	sendMsg(t, a, convID, "msg-while-b-disconnected")

	// b 重连 WS
	wsB2 := fixture.ConnectWSWithDeviceID(t, b, "device-ws06-2")
	defer wsB2.Close()

	// b 通过 sync 补齐遗漏消息
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, b.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.NotEmpty(t, syncRsp.Messages, "重连后 sync 应补齐遗漏消息")
}

// FN-WS-07 | P2 | websocket | typing 通知送达订阅者
func TestFN_WS_TypingNotify(t *testing.T) {
	a, b, convID := setupConv(t)

	// b 连接 WS
	wsB := fixture.ConnectWSWithDeviceID(t, b, "device-ws07")
	defer wsB.Close()

	// a 发 typing
	typingReq := &presence.TypingReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		IsTyping:       true,
	}
	require.NoError(t, a.DoAuth("/service/presence/send_typing", typingReq, &presence.TypingRsp{}))

	// b 应收到 TYPING_NOTIFY
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, err := wsB.WaitForNotify(ctx, int32(push.NotifyType_TYPING_NOTIFY))
	require.NoError(t, err, "b 应收到 typing 通知")
}

// FN-WS-08 | P1 | trace | Gateway trace 经 MQ 透传到接收方 WS notify
func TestFN_WS_MQTracePropagation(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	wsBob := fixture.ConnectWS(t, bob)
	defer wsBob.Close()

	traceID := "fedcba9876543210fedcba9876543210"
	req := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "trace-propagation"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	rsp := &transmite.SendMessageRsp{}
	_, err := alice.DoWithTrace("/service/transmite/send", req, rsp, alice.AccessToken, traceID)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	require.True(t, rsp.Header.Success)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err)
	assert.Equal(t, traceID, notify.GetTraceId())
}
