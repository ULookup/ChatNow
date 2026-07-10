//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

// BVT-007 | P0 | 社交链路 | A 向 B 发好友申请，返回 notify_event_id
func TestBVT_SendFriendRequest_Success(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bob.UserID,
	}
	rsp := &relationship.SendFriendRsp{}
	err := alice.DoAuth("/service/relationship/send_friend_request", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "发好友申请失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.GetNotifyEventId())
}

// BVT-008 | P0 | 社交链路 | B 通过申请，返回 new_conversation_id
func TestBVT_AcceptFriend_Success(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)

	// MakeFriends 已完成 send + accept，验证结果
	require.NotEmpty(t, convID, "通过好友申请后应返回 conversation_id")

	// 额外验证：B 的好友列表包含 A
	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID()}
	listRsp := &relationship.ListFriendsRsp{}
	err := bob.DoAuth("/service/relationship/list_friends", listReq, listRsp)
	require.NoError(t, err)
	assert.True(t, listRsp.Header.Success)
	found := false
	for _, f := range listRsp.FriendList {
		if f.UserId == alice.UserID {
			found = true
			break
		}
	}
	assert.True(t, found, "B 的好友列表应包含 A")
}
