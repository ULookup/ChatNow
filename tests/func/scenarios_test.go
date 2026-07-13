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
	"chatnow-tests/pkg/verify"
	common "chatnow-tests/proto/chatnow/common"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
	relationship "chatnow-tests/proto/chatnow/relationship"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// ---------------------------------------------------------------------------
// Scenario 1: Register → Login → Add Friend → Accept → Send First Message → Sync → GetHistory
// ---------------------------------------------------------------------------

func TestScenario_RegisterToFirstMessage(t *testing.T) {
	// Step 1: Register Alice and Bob
	alice, aliceUser, alicePwd := fixture.RegisterAndLogin(t, HTTP)
	bob, bobUser, bobPwd := fixture.RegisterAndLogin(t, HTTP)
	_, _, _, _ = aliceUser, alicePwd, bobUser, bobPwd

	// Step 2: Alice searches for Bob (by user ID prefix)
	searchReq := &identity.SearchUsersReq{RequestId: client.NewRequestID(), SearchKey: bob.UserID[:4]}
	searchRsp := &identity.SearchUsersRsp{}
	require.NoError(t, alice.DoAuth("/service/identity/search_users", searchReq, searchRsp))
	assert.True(t, searchRsp.Header.Success)

	// Step 3: Alice sends friend request to Bob
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: bob.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	assert.True(t, sendRsp.Header.Success)

	// Step 4: Bob accepts
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   alice.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, bob.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	assert.True(t, handleRsp.Header.Success)
	convID := handleRsp.GetNewConversationId()
	assert.NotEmpty(t, convID)

	// Step 5: Alice sends first message
	msgText := "Hello Bob!"
	sendMsgReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: msgText}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendMsgRsp := &transmite.SendMessageRsp{}
	require.NoError(t, alice.DoAuth("/service/transmite/send", sendMsgReq, sendMsgRsp))
	assert.True(t, sendMsgRsp.Header.Success)

	// Step 6: Bob syncs messages
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          20,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
	assert.True(t, syncRsp.Header.Success)
	assert.NotEmpty(t, syncRsp.GetMessages())

	// Step 7: Bob gets history
	histReq := &msg.GetHistoryReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		BeforeSeq:      syncRsp.GetLatestSeq() + 1,
		Limit:          20,
	}
	histRsp := &msg.GetHistoryRsp{}
	require.NoError(t, bob.DoAuth("/service/message/get_history", histReq, histRsp))
	assert.True(t, histRsp.Header.Success)
	assert.NotEmpty(t, histRsp.GetMessages())
}

// ---------------------------------------------------------------------------
// Scenario 2: Group Chat Lifecycle
// ---------------------------------------------------------------------------

func TestScenario_GroupChatLifecycle(t *testing.T) {
	// Create owner + 2 members
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// Create group
	name := "test-group-scenario"
	createReq := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &name,
		MemberIds: []string{m1.UserID, m2.UserID},
	}
	createRsp := &conversation.CreateConversationRsp{}
	require.NoError(t, owner.DoAuth("/service/conversation/create", createReq, createRsp))
	convID := createRsp.Conversation.ConversationId

	// Send image message with @mention
	sendReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_IMAGE,
			Body: &msg.MessageContent_Image{Image: &msg.ImageContent{
				FileId:       "fake-img",
				Width:        100,
				Height:       100,
				ThumbnailUrl: "http://x.com/t.jpg",
			}},
		},
		ClientMsgId:      client.NewRequestID(),
		MentionedUserIds: []string{m1.UserID},
	}
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, owner.DoAuth("/service/transmite/send", sendReq, sendRsp))
	msgID := sendRsp.Message.MessageId

	// Add reaction
	reactReq := &msg.AddReactionReq{RequestId: client.NewRequestID(), MessageId: msgID, Emoji: "🔥"}
	reactRsp := &msg.AddReactionRsp{}
	require.NoError(t, m1.DoAuth("/service/message/add_reaction", reactReq, reactRsp))
	assert.True(t, reactRsp.Header.Success)

	// Recall message
	recallReq := &msg.RecallMessageReq{RequestId: client.NewRequestID(), ConversationId: convID, MessageId: msgID}
	recallRsp := &msg.RecallMessageRsp{}
	require.NoError(t, owner.DoAuth("/service/message/recall", recallReq, recallRsp))
	assert.True(t, recallRsp.Header.Success)

	// Dismiss group
	dismissReq := &conversation.DismissConversationReq{RequestId: client.NewRequestID(), ConversationId: convID}
	dismissRsp := &conversation.DismissConversationRsp{}
	require.NoError(t, owner.DoAuth("/service/conversation/dismiss", dismissReq, dismissRsp))
	assert.True(t, dismissRsp.Header.Success)
}

// ---------------------------------------------------------------------------
// Scenario 3: Friend Full Lifecycle
// ---------------------------------------------------------------------------

func TestScenario_FriendFullLifecycle(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// A sends friend request to B — B accepts
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	convID := handleRsp.GetNewConversationId()
	assert.NotEmpty(t, convID)

	// Exchange messages
	sendMsgReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hey"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendMsgRsp := &transmite.SendMessageRsp{}
	require.NoError(t, a.DoAuth("/service/transmite/send", sendMsgReq, sendMsgRsp))
	assert.True(t, sendMsgRsp.Header.Success)

	// Remove friend
	removeReq := &relationship.RemoveFriendReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	removeRsp := &relationship.RemoveFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/remove_friend", removeReq, removeRsp))
	assert.True(t, removeRsp.Header.Success)

	// Verify friend list empty
	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID(), Page: &common.PageRequest{Limit: 20}}
	listRsp := &relationship.ListFriendsRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/list_friends", listReq, listRsp))
	assert.Empty(t, listRsp.FriendList)
}

// ---------------------------------------------------------------------------
// Scenario 4: Offline Message Sync（离线消息同步）
// SC-04 | P0 | u2 离线 -> u1 发 3 条 -> u2 上线 sync -> WS 实时推送
// ---------------------------------------------------------------------------

func TestScenario_OfflineMessageSync(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, bobUser, bobPwd := fixture.RegisterAndLogin(t, HTTP)

	// Step 1: bob 登出（模拟离线）
	logoutReq := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bob.DoAuth("/service/identity/logout", logoutReq, &identity.LogoutRsp{}))

	// Step 2: alice 发好友申请 -> bob 重新登录后处理
	// 注：bob 已登出，需要重新登录后才能接受好友申请
	// 改为：先加好友，再登出
	_ = bobUser
	_ = bobPwd

	// 重新设计：先加好友，再登出
	bobRelogin := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// alice 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bobRelogin.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// bob 接受
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   alice.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, bobRelogin.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	require.True(t, handleRsp.Header.Success)
	convID := handleRsp.GetNewConversationId()
	require.NotEmpty(t, convID)

	// bob 登出（模拟离线）
	logoutReq2 := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bobRelogin.DoAuth("/service/identity/logout", logoutReq2, &identity.LogoutRsp{}))

	// Step 3: alice 发 3 条消息（bob 离线）
	texts := []string{"offline-1", "offline-2", "offline-3"}
	var lastSeq uint64
	for _, txt := range texts {
		_, seq := fixture.SendTextMessage(t, alice, convID, txt)
		lastSeq = seq
	}

	// Step 4: bob 重新登录
	bobOnline := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// Step 5: bob sync，验证 3 条按序到达
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          20,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 3, "应返回 3 条离线消息")

	for i, m := range syncRsp.Messages {
		assert.Equal(t, texts[i], m.GetContent().GetText().Text, "第 %d 条消息内容不匹配", i+1)
		if i > 0 {
			assert.Less(t, syncRsp.Messages[i-1].SeqId, m.SeqId, "seq 应递增")
		}
	}

	// Step 6: bob 开 WS，不应收到旧消息推送（已通过 sync 拉取）
	wsBob := fixture.ConnectWS(t, bobOnline)
	defer wsBob.Close()
	time.Sleep(1 * time.Second) // 等 WS 鉴权完成

	// Step 7: alice 再发 1 条，bob WS 应收到实时推送
	fixture.SendTextMessage(t, alice, convID, "realtime-msg")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err, "应收到实时消息推送")
	actualMsg := notify.GetNewMessageInfo().GetMessageInfo()
	if actualMsg != nil {
		assert.Equal(t, "realtime-msg", actualMsg.GetContent().GetText().Text)
	}

	// Step 8: bob 增量 sync，仅返回新 1 条
	syncReq2 := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       lastSeq,
		Limit:          20,
	}
	syncRsp2 := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq2, syncRsp2))
	require.True(t, syncRsp2.Header.Success)
	require.Len(t, syncRsp2.Messages, 1, "增量 sync 应仅返回 1 条新消息")

	// Step 9: 数据一致性 - 直查 DB
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 4) // 3 离线 + 1 实时 = 4
}

// ---------------------------------------------------------------------------
// Scenario 6: Message Reliability（MQ 可用版本）
// SC-06 | P0 | client_msg_id 幂等 + 消息不丢不重
// 注：Phase 1 不做 MQ stop/start（那是 RL-01 的职责），
// 此版本验证 MQ 正常可用时的 client_msg_id 幂等机制。
// ---------------------------------------------------------------------------

func TestScenario_MessageReliability(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	// Step 1: alice 发消息，获得 message_id
	clientMsgID := client.NewRequestID()
	msgID1, seq1, success1 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)
	require.True(t, success1, "第一次发送应成功")
	require.NotZero(t, msgID1)

	// Step 2: 用相同 client_msg_id 重发（模拟网络重传）
	msgID2, seq2, success2 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)

	// 幂等验证
	if success2 {
		assert.Equal(t, msgID1, msgID2, "相同 client_msg_id 应返回相同 message_id")
		assert.Equal(t, seq1, seq2, "相同 client_msg_id 应返回相同 seq_id")
	}

	// Step 3: bob sync 验证收到该消息（仅 1 条）
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 1, "应仅收到 1 条消息（幂等去重）")
	assert.Equal(t, msgID1, syncRsp.Messages[0].MessageId)
	assert.Equal(t, "reliability-test", syncRsp.Messages[0].GetContent().GetText().Text)

	// Step 4: SelectByClientMsgId 验证可查到
	selectReq := &msg.SelectByClientMsgIdReq{
		RequestId:   client.NewRequestID(),
		ClientMsgId: clientMsgID,
	}
	selectRsp := &msg.SelectByClientMsgIdRsp{}
	require.NoError(t, alice.DoAuth("/service/message/select_by_client_msg_id", selectReq, selectRsp))
	require.True(t, selectRsp.Header.Success)
	require.NotNil(t, selectRsp.Message)
	assert.Equal(t, msgID1, selectRsp.Message.MessageId)

	// Step 5: 数据一致性 - DB 仅 1 条（不重复）
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}
