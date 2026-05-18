//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	presence "chatnow-tests/proto/chatnow/presence"
)

func TestGetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.GetPresenceReq{RequestId: client.NewRequestID(), UserId: other.UserID}
	rsp := &presence.GetPresenceRsp{}
	err := authed.DoAuth("/service/presence/get", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestBatchGetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.BatchGetPresenceReq{
		RequestId: client.NewRequestID(), UserIds: []string{authed.UserID, other.UserID},
	}
	rsp := &presence.BatchGetPresenceRsp{}
	err := authed.DoAuth("/service/presence/batch_get", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestSubscribePresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &presence.SubscribeReq{
		RequestId: client.NewRequestID(), SubscribeUserIds: []string{other.UserID},
	}
	rsp := &presence.SubscribeRsp{}
	err := authed.DoAuth("/service/presence/subscribe", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestUnsubscribePresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	other, _, _ := fixture.RegisterAndLogin(t, HTTP)
	subReq := &presence.SubscribeReq{RequestId: client.NewRequestID(), SubscribeUserIds: []string{other.UserID}}
	require.NoError(t, authed.DoAuth("/service/presence/subscribe", subReq, &presence.SubscribeRsp{}))
	req := &presence.UnsubscribeReq{
		RequestId: client.NewRequestID(), UnsubscribeUserIds: []string{other.UserID},
	}
	rsp := &presence.UnsubscribeRsp{}
	err := authed.DoAuth("/service/presence/unsubscribe", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}
