//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
)

// FN-SEC-01 | P0 | 安全 | 无 token 访问受保护接口应被拒绝
func TestFN_SEC_AuthBypass_NoToken(t *testing.T) {
	// 无 token 调用 GetProfile（受保护接口）
	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := HTTP.DoNoAuth("/service/identity/get_profile", req, rsp)

	// 预期：HTTP 错误（401/403）或 protobuf 响应 success=false
	if err != nil {
		// HTTP-level rejection (401/403) - expected
		return
	}
	require.False(t, rsp.Header.Success, "无 token 请求应被拒绝")
}

// FN-SEC-02 | P0 | 安全 | 用 A 的 token 访问 B 的数据应被拒绝
func TestFN_SEC_AuthBypass_OtherUser(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// alice 和 bob 不是好友，也没有共同会话
	// alice 尝试用 token 访问 bob 的数据
	// 尝试 1：alice 调 SyncMessages（bob 不在的会话）
	// 先让 bob 建一个会话
	bobFriend, _, convID := fixture.MakeFriends(t, bob)

	// alice 尝试 sync bob 的会话
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	err := alice.DoAuth("/service/message/sync", syncReq, syncRsp)
	require.NoError(t, err)
	require.False(t, syncRsp.Header.Success, "alice 不应用能 sync bob 的会话")
	assert.Equal(t, int32(3002), syncRsp.Header.ErrorCode, "错误码应为 CONVERSATION_NOT_MEMBER(3002)")

	_ = bobFriend
}

// FN-SEC-06 | P0 | 安全 | 普通成员尝试改自己为群主应被拒绝
func TestFN_SEC_PrivilegeEscalation_MemberToOwner(t *testing.T) {
	owner, members, convID := fixture.CreateGroupSimple(t, HTTP, 2)
	member := members[0]

	// member 尝试将自己角色改为 OWNER
	req := &conversation.ChangeMemberRoleReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		TargetUserId:   member.UserID,
		Role:           conversation.MemberRole_OWNER,
	}
	rsp := &conversation.ChangeMemberRoleRsp{}
	err := member.DoAuth("/service/conversation/change_role", req, rsp)
	require.NoError(t, err)
	require.False(t, rsp.Header.Success, "普通成员不能改自己为群主")
	assert.Equal(t, int32(3003), rsp.Header.ErrorCode, "错误码应为 CONVERSATION_NO_PERMISSION(3003)")

	// 直查 DB 验证 member 仍是 MEMBER 角色
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.ConversationMemberRole(t, member.UserID, convID, 0) // 0=MEMBER

	// owner 仍是 OWNER
	dbV.ConversationMemberRole(t, owner.UserID, convID, 2) // 2=OWNER
}
