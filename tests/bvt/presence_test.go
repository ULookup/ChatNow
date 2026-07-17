//go:build bvt

package bvt_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	presence "chatnow-tests/proto/chatnow/presence"
)

// BVT-018 | P0 | presence 链路 | 查询在线状态，返回 online
func TestBVT_GetPresence_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 登录后 presence 应为 ONLINE
	req := &presence.GetPresenceReq{
		RequestId: client.NewRequestID(),
		UserId:    authed.UserID,
	}
	rsp := &presence.GetPresenceRsp{}
	err := authed.DoAuth("/service/presence/get", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "get_presence 失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.Presence)
	assert.Equal(t, presence.PresenceState_ONLINE, rsp.Presence.AggregatedState,
		"登录后 presence 应为 ONLINE，实际 %v", rsp.Presence.AggregatedState)
}
