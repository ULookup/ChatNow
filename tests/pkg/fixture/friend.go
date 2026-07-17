package fixture

import (
	"testing"

	"chatnow-tests/pkg/client"
	relationship "chatnow-tests/proto/chatnow/relationship"
)

// MakeFriends creates two users, sends a friend request from a→b, and b accepts.
// Returns both authed clients and the new conversation ID.
func MakeFriends(t testing.TB, base *client.HTTPClient) (a, b *client.HTTPClient, convID string) {
	a, _, _ = RegisterAndLogin(t, base)
	b, _, _ = RegisterAndLogin(t, base)

	// a sends friend request to b
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: b.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	if err := a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp); err != nil {
		t.Fatalf("SendFriendRequest: %v", err)
	}
	if !sendRsp.Header.Success {
		t.Fatalf("SendFriendRequest failed: code=%d msg=%s", sendRsp.Header.ErrorCode, sendRsp.Header.ErrorMessage)
	}

	eventID := sendRsp.GetNotifyEventId()

	// b accepts
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: eventID,
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	if err := b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp); err != nil {
		t.Fatalf("HandleFriendRequest: %v", err)
	}
	if !handleRsp.Header.Success {
		t.Fatalf("HandleFriendRequest failed: code=%d msg=%s", handleRsp.Header.ErrorCode, handleRsp.Header.ErrorMessage)
	}
	return a, b, handleRsp.GetNewConversationId()
}
