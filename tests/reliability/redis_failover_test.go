//go:build reliability

package reliability_test

import (
	"fmt"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// RL-05 | P0 | Redis 熔断后快速失败并自动恢复
func TestRL_RedisCircuitFastFailAndRecovery(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	peer, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, user, []*client.HTTPClient{peer}, "rl-redis-outage")
	prewarmRsp := &transmite.SendMessageRsp{}
	require.NoError(t, user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "prewarm"}},
		},
		ClientMsgId: client.NewRequestID(),
	}, prewarmRsp))
	require.True(t, prewarmRsp.GetHeader().GetSuccess())

	t.Cleanup(func() { chaos.StartRedisCluster(t); chaos.WaitRedisCluster(t, 60*time.Second) })
	chaos.StopRedisCluster(t)

	rateLimited := 0
	for i := 0; i < 650; i++ {
		sendRsp := &transmite.SendMessageRsp{}
		err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
			RequestId:      client.NewRequestID(),
			ConversationId: convID,
			Content: &msg.MessageContent{
				Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("outage-%d", i)}},
			},
			ClientMsgId: client.NewRequestID(),
		}, sendRsp)
		require.NoError(t, err)
		if sendRsp.GetHeader().GetErrorMessage() == "rate_limited" {
			rateLimited++
		}
	}
	require.Greater(t, rateLimited, 0, "Redis outage must retain bounded rate limiting")

	uid := user.UserID
	for i := 0; i < 3; i++ {
		rsp := &identity.GetProfileRsp{}
		_ = user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
			RequestId: client.NewRequestID(), UserId: &uid,
		}, rsp)
	}

	started := time.Now()
	rsp := &identity.GetProfileRsp{}
	err := user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(), UserId: &uid,
	}, rsp)
	require.NoError(t, err)
	require.True(t, rsp.GetHeader().GetSuccess())
	require.Less(t, time.Since(started), 50*time.Millisecond)

	chaos.StartRedisCluster(t)
	chaos.WaitRedisCluster(t, 60*time.Second)
	time.Sleep(1100 * time.Millisecond)
	rsp = &identity.GetProfileRsp{}
	require.NoError(t, user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(), UserId: &uid,
	}, rsp))
	require.True(t, rsp.GetHeader().GetSuccess())
}
