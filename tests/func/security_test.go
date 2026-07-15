//go:build func

package func_test

import (
	"crypto/sha256"
	"fmt"
	"regexp"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	media "chatnow-tests/proto/chatnow/media"
	msg "chatnow-tests/proto/chatnow/message"
	relationship "chatnow-tests/proto/chatnow/relationship"
	transmite "chatnow-tests/proto/chatnow/transmite"
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

// FN-SEC-03 | P1 | security | 搜索接口 SQL 注入：注入 payload 不应破坏查询
func TestFN_SEC_SQLInjection_Search(t *testing.T) {
	a, b, convID := fixture.MakeFriends(t, HTTP)

	// 先发一条正常消息
	sendReq := &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "normal message"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	require.NoError(t, a.DoAuth("/service/transmite/send", sendReq, &transmite.SendMessageRsp{}))

	// 用 SQL 注入 payload 搜索好友
	injectionPayloads := []string{
		"'; DROP TABLE friend; --",
		"' OR '1'='1",
		"' UNION SELECT * FROM user; --",
	}
	for _, payload := range injectionPayloads {
		req := &relationship.SearchFriendsReq{RequestId: client.NewRequestID(), SearchKey: payload}
		rsp := &relationship.SearchFriendsRsp{}
		require.NoError(t, b.DoAuth("/service/relationship/search_friends", req, rsp),
			"SQL 注入 payload 不应导致请求失败: %s", payload)
		require.NotNil(t, rsp.Header)
		require.True(t, rsp.Header.Success)
		require.Empty(t, rsp.UserInfo, "SQL 注入 payload 应按普通字面量搜索: %s", payload)
	}

	// 验证 friend 表未被破坏（仍能 ListFriends）
	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID()}
	listRsp := &relationship.ListFriendsRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/list_friends", listReq, listRsp))
	require.NotNil(t, listRsp.Header)
	require.True(t, listRsp.Header.Success, "SQL 注入后 friend 表应完好")
}

// FN-SEC-04 | P1 | security | 消息内容含 XSS payload，应被转义/存储为原始文本
func TestFN_SEC_XSS_MessageContent(t *testing.T) {
	a, _, convID := fixture.MakeFriends(t, HTTP)

	xssPayload := "<script>alert('xss')</script>"
	sendReq := &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: xssPayload}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, a.DoAuth("/service/transmite/send", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success, "XSS payload 应作为文本存储（不拒绝）")

	// 同步消息，验证内容原样返回（服务端不执行转义，客户端负责）
	syncReq := &msg.SyncMessagesReq{RequestId: client.NewRequestID(), ConversationId: convID, AfterSeq: 0, Limit: 10}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, a.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.NotEmpty(t, syncRsp.Messages)
	// 最后一条消息内容应与发送的 payload 一致（存储为原始文本）
	lastMsg := syncRsp.Messages[len(syncRsp.Messages)-1]
	assert.Equal(t, xssPayload, lastMsg.GetContent().GetText().Text, "XSS payload 应原样存储")
}

// FN-SEC-05 | P1 | security | 文件名含路径遍历字符，对象路径仍仅由 purpose/hash 派生
func TestFN_SEC_PathTraversal_FileName(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	chatObjectKeyPattern := regexp.MustCompile(`^chat/[0-9]{4}/[0-9]{2}/[0-9]{2}/[0-9a-f]{2}/[0-9a-f]{64}$`)

	traversalNames := []string{
		"../../etc/passwd",
		"..\\..\\windows\\system32",
		"./../../secret",
	}
	for _, name := range traversalNames {
		uniqueContent := []byte(name + client.NewRequestID())
		hash := sha256.Sum256(uniqueContent)
		req := &media.ApplyUploadReq{
			RequestId: client.NewRequestID(), FileName: name,
			FileSize: int64(len(uniqueContent)), MimeType: "text/plain",
			ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
		}
		rsp := &media.ApplyUploadRsp{}
		require.NoError(t, authed.DoAuth("/service/media/apply_upload", req, rsp),
			"路径遍历文件名不应导致请求崩溃: %s", name)
		require.NotNil(t, rsp.Header)
		require.True(t, rsp.Header.Success, "文件显示名不应影响对象路径派生: %s", name)
		require.NotEmpty(t, rsp.FileId)

		record := dbV.MediaFile(t, rsp.FileId)
		assert.NotContains(t, record.ObjectKey, "..")
		assert.NotContains(t, record.ObjectKey, `\`)
		assert.Regexp(t, chatObjectKeyPattern, record.ObjectKey)
	}
}
