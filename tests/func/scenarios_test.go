//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	common "chatnow-tests/proto/chatnow/common"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
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

	// First request — b rejects
	sendReq1 := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp1 := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq1, sendRsp1))
	handleReq1 := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp1.GetNotifyEventId(),
		Agree:         false,
		ApplyUserId:   a.UserID,
	}
	handleRsp1 := &relationship.HandleFriendRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq1, handleRsp1))
	assert.Empty(t, handleRsp1.GetNewConversationId())

	// Re-apply — b accepts
	sendReq2 := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp2 := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq2, sendRsp2))
	handleReq2 := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp2.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp2 := &relationship.HandleFriendRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq2, handleRsp2))
	convID := handleRsp2.GetNewConversationId()

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
