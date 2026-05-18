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
)

// ---------------------------------------------------------------------------
// 1. ListConversations
// ---------------------------------------------------------------------------

func TestListConversations_Empty(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &conversation.ListConversationsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 20},
	}
	rsp := &conversation.ListConversationsRsp{}
	err := authed.DoAuth("/service/conversation/list", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestListConversations_AfterFriendAccept(t *testing.T) {
	a, _, convID := fixture.MakeFriends(t, HTTP)
	req := &conversation.ListConversationsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 20},
	}
	rsp := &conversation.ListConversationsRsp{}
	err := a.DoAuth("/service/conversation/list", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	found := false
	for _, c := range rsp.Conversations {
		if c.ConversationId == convID {
			found = true
			break
		}
	}
	assert.True(t, found, "list should contain the newly created private conversation")
}

// ---------------------------------------------------------------------------
// 2. GetConversation
// ---------------------------------------------------------------------------

func TestGetConversation_Success(t *testing.T) {
	a, _, convID := fixture.MakeFriends(t, HTTP)
	req := &conversation.GetConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.GetConversationRsp{}
	err := a.DoAuth("/service/conversation/get", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Conversation)
	assert.Equal(t, convID, rsp.Conversation.ConversationId)
	assert.Equal(t, conversation.ConversationType_PRIVATE, rsp.Conversation.Type)
}

func TestGetConversation_NotFound(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &conversation.GetConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: "nonexistent_conv_id_12345",
	}
	rsp := &conversation.GetConversationRsp{}
	err := authed.DoAuth("/service/conversation/get", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3001), rsp.Header.ErrorCode)
}

func TestGetConversation_NotMember(t *testing.T) {
	a, _, convID := fixture.MakeFriends(t, HTTP)
	// Register a third user who is not part of the conversation.
	third, _, _ := fixture.RegisterAndLogin(t, HTTP)
	_ = a // friend a
	req := &conversation.GetConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.GetConversationRsp{}
	err := third.DoAuth("/service/conversation/get", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3002), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 3. CreateConversation
// ---------------------------------------------------------------------------

func TestCreateConversation_Group_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	groupName := "test_group"
	req := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &groupName,
		MemberIds: []string{member1.UserID, member2.UserID},
	}
	rsp := &conversation.CreateConversationRsp{}
	err := owner.DoAuth("/service/conversation/create", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Conversation)
	assert.NotEmpty(t, rsp.Conversation.ConversationId)
	assert.Equal(t, conversation.ConversationType_GROUP, rsp.Conversation.Type)
	assert.Equal(t, int32(3), rsp.Conversation.MemberCount)
	require.NotNil(t, rsp.Conversation.Self)
	assert.Equal(t, conversation.MemberRole_OWNER, rsp.Conversation.Self.Role)
}

// ---------------------------------------------------------------------------
// 4. UpdateConversation
// ---------------------------------------------------------------------------

func TestUpdateConversation_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "original_name")

	newName := "updated_name"
	req := &conversation.UpdateConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Name:           &newName,
	}
	rsp := &conversation.UpdateConversationRsp{}
	err := owner.DoAuth("/service/conversation/update", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Conversation)
	assert.Equal(t, newName, rsp.Conversation.Name)
}

func TestUpdateConversation_NoPermission(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	newName := "hacked_name"
	req := &conversation.UpdateConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Name:           &newName,
	}
	rsp := &conversation.UpdateConversationRsp{}
	err := member.DoAuth("/service/conversation/update", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 5. AddMembers / RemoveMembers
// ---------------------------------------------------------------------------

func TestAddMembers_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	newMember, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member1}, "test_group")

	req := &conversation.AddMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MemberIds:      []string{newMember.UserID},
	}
	rsp := &conversation.AddMembersRsp{}
	err := owner.DoAuth("/service/conversation/add_members", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestRemoveMembers_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member2, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member1, member2}, "test_group")

	req := &conversation.RemoveMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MemberIds:      []string{member2.UserID},
	}
	rsp := &conversation.RemoveMembersRsp{}
	err := owner.DoAuth("/service/conversation/remove_members", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 6. TransferOwner
// ---------------------------------------------------------------------------

func TestTransferOwner_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.TransferOwnerReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		NewOwnerId:     member.UserID,
	}
	rsp := &conversation.TransferOwnerRsp{}
	err := owner.DoAuth("/service/conversation/transfer_owner", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 7. DismissConversation
// ---------------------------------------------------------------------------

func TestDismissConversation_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.DismissConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.DismissConversationRsp{}
	err := owner.DoAuth("/service/conversation/dismiss", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestDismissConversation_NotOwner(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.DismissConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.DismissConversationRsp{}
	err := member.DoAuth("/service/conversation/dismiss", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 8. ChangeMemberRole
// ---------------------------------------------------------------------------

func TestChangeMemberRole_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.ChangeMemberRoleReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		TargetUserId:   member.UserID,
		Role:           conversation.MemberRole_ADMIN,
	}
	rsp := &conversation.ChangeMemberRoleRsp{}
	err := owner.DoAuth("/service/conversation/change_role", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 9. ListMembers
// ---------------------------------------------------------------------------

func TestListMembers_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.ListMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Page:           &common.PageRequest{Limit: 50},
	}
	rsp := &conversation.ListMembersRsp{}
	err := owner.DoAuth("/service/conversation/list_members", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	assert.Len(t, rsp.Members, 2)
}

// ---------------------------------------------------------------------------
// 10. SetMute
// ---------------------------------------------------------------------------

func TestSetMute_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.SetMuteReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Mute:           true,
	}
	rsp := &conversation.SetMuteRsp{}
	err := owner.DoAuth("/service/conversation/set_mute", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Self)
	assert.True(t, rsp.Self.IsMuted)
}

// ---------------------------------------------------------------------------
// 11. SetPin
// ---------------------------------------------------------------------------

func TestSetPin_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.SetPinReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Pin:            true,
	}
	rsp := &conversation.SetPinRsp{}
	err := owner.DoAuth("/service/conversation/set_pin", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Self)
	assert.True(t, rsp.Self.IsPinned)
}

// ---------------------------------------------------------------------------
// 12. SetVisible
// ---------------------------------------------------------------------------

func TestSetVisible_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.SetVisibleReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Visible:        false,
	}
	rsp := &conversation.SetVisibleRsp{}
	err := owner.DoAuth("/service/conversation/set_visible", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Self)
	assert.False(t, rsp.Self.IsVisible)
}

// ---------------------------------------------------------------------------
// 13. QuitConversation
// ---------------------------------------------------------------------------

func TestQuitConversation_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.QuitConversationReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	rsp := &conversation.QuitConversationRsp{}
	err := member.DoAuth("/service/conversation/quit", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 14. MarkRead
// ---------------------------------------------------------------------------

func TestMarkRead_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	req := &conversation.MarkReadReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		LastReadSeq:    100,
	}
	rsp := &conversation.MarkReadRsp{}
	err := owner.DoAuth("/service/conversation/mark_read", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 15. SaveDraft
// ---------------------------------------------------------------------------

func TestSaveDraft_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	draftText := "hello draft"
	req := &conversation.SaveDraftReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Draft:          draftText,
	}
	rsp := &conversation.SaveDraftRsp{}
	err := owner.DoAuth("/service/conversation/save_draft", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.Self)
	assert.Equal(t, draftText, rsp.Self.GetDraft())
}

// ---------------------------------------------------------------------------
// 16. SearchConversations
// ---------------------------------------------------------------------------

func TestSearchConversations_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	member, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, owner, []*client.HTTPClient{member}, "test_group")

	// Search by a partial substring of the conversation ID.
	searchKey := convID[2:8]
	req := &conversation.SearchConversationsReq{
		RequestId: client.NewRequestID(),
		SearchKey: searchKey,
	}
	rsp := &conversation.SearchConversationsRsp{}
	err := owner.DoAuth("/service/conversation/search", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	found := false
	for _, c := range rsp.Conversations {
		if c.ConversationId == convID {
			found = true
			break
		}
	}
	assert.True(t, found, "search by partial convID should find the conversation")
}
