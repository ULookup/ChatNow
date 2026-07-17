package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
	conversation "chatnow-tests/proto/chatnow/conversation"
)

// CreateGroupWithMembers creates a group conversation with the given members.
func CreateGroupWithMembers(t testing.TB, owner *client.HTTPClient, members []*client.HTTPClient, name string) string {
	memberIDs := make([]string, len(members))
	for i, m := range members {
		memberIDs[i] = m.UserID
	}

	req := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &name,
		MemberIds: memberIDs,
	}
	rsp := &conversation.CreateConversationRsp{}
	if err := owner.DoAuth("/service/conversation/create", req, rsp); err != nil {
		t.Fatalf("CreateConversation: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("CreateConversation failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
	return rsp.Conversation.ConversationId
}
