package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
	conversation "chatnow-tests/proto/chatnow/conversation"
)

// CreateGroup 创建群会话（owner + members），返回 conversation_id。
// 注：已有 CreateGroupWithMembers 在 conversation.go 中，此为简化别名。
func CreateGroup(t testing.TB, owner *client.HTTPClient, members []*client.HTTPClient, name string) string {
	return CreateGroupWithMembers(t, owner, members, name)
}

// AddMembers 向群会话添加成员。
func AddMembers(t testing.TB, owner *client.HTTPClient, convID string, memberIDs []string) {
	req := &conversation.AddMembersReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MemberIds:      memberIDs,
	}
	rsp := &conversation.AddMembersRsp{}
	if err := owner.DoAuth("/service/conversation/add_members", req, rsp); err != nil {
		t.Fatalf("AddMembers: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("AddMembers failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
}

// CreateGroupSimple 创建群会话并返回 convID + 所有成员 client（含 owner）。
func CreateGroupSimple(t testing.TB, base *client.HTTPClient, memberCount int) (owner *client.HTTPClient, members []*client.HTTPClient, convID string) {
	owner, _, _ = RegisterAndLogin(t, base)
	members = make([]*client.HTTPClient, 0, memberCount)
	memberIDs := make([]string, 0, memberCount)
	for i := 0; i < memberCount; i++ {
		m, _, _ := RegisterAndLogin(t, base)
		members = append(members, m)
		memberIDs = append(memberIDs, m.UserID)
	}
	name := "test-group-" + client.NewRequestID()[:8]
	convID = CreateGroup(t, owner, members, name)
	return owner, members, convID
}
