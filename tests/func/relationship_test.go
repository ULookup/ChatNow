//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	common "chatnow-tests/proto/chatnow/common"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

func TestListFriends_Empty(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &relationship.ListFriendsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 20},
	}
	rsp := &relationship.ListFriendsRsp{}
	err := authed.DoAuth("/service/relationship/list_friends", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	assert.Empty(t, rsp.FriendList)
}

func TestListFriends_WithFriends(t *testing.T) {
	a, b, _ := fixture.MakeFriends(t, HTTP)
	req := &relationship.ListFriendsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 20},
	}
	rsp := &relationship.ListFriendsRsp{}
	err := a.DoAuth("/service/relationship/list_friends", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	assert.Len(t, rsp.FriendList, 1)
	assert.Equal(t, b.UserID, rsp.FriendList[0].UserId)
}

func TestSendFriendRequest_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: b.UserID,
	}
	rsp := &relationship.SendFriendRsp{}
	err := a.DoAuth("/service/relationship/send_friend_request", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.GetNotifyEventId())
}

func TestSendFriendRequest_AlreadyFriends_Error(t *testing.T) {
	a, b, _ := fixture.MakeFriends(t, HTTP)
	req := &relationship.SendFriendReq{
		RequestId: client.NewRequestID(), RespondentId: b.UserID,
	}
	rsp := &relationship.SendFriendRsp{}
	err := a.DoAuth("/service/relationship/send_friend_request", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(2001), rsp.Header.ErrorCode)
}

func TestSendFriendRequest_DuplicatePending_Error(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req1 := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	rsp1 := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", req1, rsp1))
	require.True(t, rsp1.Header.Success)

	req2 := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	rsp2 := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", req2, rsp2))
	assert.False(t, rsp2.Header.Success)
	assert.Equal(t, int32(2004), rsp2.Header.ErrorCode)
}

func TestSendFriendRequest_Blocked_Error(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	blockReq := &relationship.BlockUserReq{RequestId: client.NewRequestID(), PeerId: a.UserID}
	blockRsp := &relationship.BlockUserRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/block_user", blockReq, blockRsp))
	require.True(t, blockRsp.Header.Success)

	req := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	rsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", req, rsp))
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(2003), rsp.Header.ErrorCode)
}

func TestHandleFriendRequest_Accept(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	err := b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp)
	require.NoError(t, err)
	assert.True(t, handleRsp.Header.Success)
	assert.NotEmpty(t, handleRsp.GetNewConversationId())
}

func TestHandleFriendRequest_Reject(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))

	handleReq := &relationship.HandleFriendReq{
		RequestId: client.NewRequestID(), NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree: false, ApplyUserId: a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	err := b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp)
	require.NoError(t, err)
	assert.True(t, handleRsp.Header.Success)
	assert.Empty(t, handleRsp.GetNewConversationId())
}

func TestRemoveFriend_Success(t *testing.T) {
	a, b, _ := fixture.MakeFriends(t, HTTP)
	req := &relationship.RemoveFriendReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	rsp := &relationship.RemoveFriendRsp{}
	err := a.DoAuth("/service/relationship/remove_friend", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)

	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID(), Page: &common.PageRequest{Limit: 20}}
	listRsp := &relationship.ListFriendsRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/list_friends", listReq, listRsp))
	assert.Empty(t, listRsp.FriendList)
}

func TestRemoveFriend_NotFriends_Error(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &relationship.RemoveFriendReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	rsp := &relationship.RemoveFriendRsp{}
	err := a.DoAuth("/service/relationship/remove_friend", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(2002), rsp.Header.ErrorCode)
}

func TestBlockUser_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &relationship.BlockUserReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	rsp := &relationship.BlockUserRsp{}
	err := a.DoAuth("/service/relationship/block_user", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestUnblockUser_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	blockReq := &relationship.BlockUserReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	blockRsp := &relationship.BlockUserRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/block_user", blockReq, blockRsp))

	unblockReq := &relationship.UnblockUserReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	unblockRsp := &relationship.UnblockUserRsp{}
	err := a.DoAuth("/service/relationship/unblock_user", unblockReq, unblockRsp)
	require.NoError(t, err)
	assert.True(t, unblockRsp.Header.Success)
}

func TestListBlockedUsers_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	blockReq := &relationship.BlockUserReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	blockRsp := &relationship.BlockUserRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/block_user", blockReq, blockRsp))

	listReq := &relationship.ListBlockedReq{RequestId: client.NewRequestID(), Page: &common.PageRequest{Limit: 20}}
	listRsp := &relationship.ListBlockedRsp{}
	err := a.DoAuth("/service/relationship/list_blocked", listReq, listRsp)
	require.NoError(t, err)
	assert.True(t, listRsp.Header.Success)
	assert.Len(t, listRsp.BlockedList, 1)
}

func TestListPendingRequests_Success(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))

	req := &relationship.ListPendingReq{RequestId: client.NewRequestID()}
	rsp := &relationship.ListPendingRsp{}
	err := b.DoAuth("/service/relationship/list_pending", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.Event)
}

func TestSearchFriends_Success(t *testing.T) {
	a, b, _ := fixture.MakeFriends(t, HTTP)
	req := &relationship.SearchFriendsReq{RequestId: client.NewRequestID(), SearchKey: b.UserID[:4]}
	rsp := &relationship.SearchFriendsRsp{}
	err := a.DoAuth("/service/relationship/search_friends", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}
