//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	common "chatnow-tests/proto/chatnow/common"
	conversation "chatnow-tests/proto/chatnow/conversation"
)

// BVT-012 | P0 | 会话链路 | 创建群会话，返回 conversation_id
func TestBVT_CreateGroupConversation_Success(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	name := "bvt-group-" + client.NewRequestID()[:8]
	req := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &name,
		MemberIds: []string{m1.UserID, m2.UserID},
	}
	rsp := &conversation.CreateConversationRsp{}
	err := owner.DoAuth("/service/conversation/create", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "创建群会话失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.Conversation)
	assert.NotEmpty(t, rsp.Conversation.ConversationId)
}

// BVT-013 | P0 | 会话链路 | 添加成员到群会话
func TestBVT_AddMembers_Success(t *testing.T) {
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 2)

	// 添加第 3 个成员
	m3, _, _ := fixture.RegisterAndLogin(t, HTTP)
	fixture.AddMembers(t, owner, convID, []string{m3.UserID})

	// 验证成员数 = 3（owner + 2 初始 + 1 新增）
	listReq := &conversation.ListMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
	}
	listRsp := &conversation.ListMembersRsp{}
	err := owner.DoAuth("/service/conversation/list_members", listReq, listRsp)
	require.NoError(t, err)
	assert.True(t, listRsp.Header.Success)
	assert.Len(t, listRsp.Members, 3)

	_ = members
}

// BVT-014 | P0 | 会话链路 | 列出会话，包含刚建的群
func TestBVT_ListConversations_Success(t *testing.T) {
	owner, _, convID := fixture.CreateGroupSimple(t, HTTP, 1)

	req := &conversation.ListConversationsReq{
		RequestId: client.NewRequestID(),
		Page:      &common.PageRequest{Limit: 50},
	}
	rsp := &conversation.ListConversationsRsp{}
	err := owner.DoAuth("/service/conversation/list", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "list conversations 失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.Conversations)

	// 验证列表包含刚建的群
	found := false
	for _, c := range rsp.Conversations {
		if c.ConversationId == convID {
			found = true
			break
		}
	}
	assert.True(t, found, "会话列表应包含刚建的群 %s", convID)
}
