//go:build reliability

package reliability_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// RL-05 | P0 | Redis 熔断后快速失败并自动恢复
func TestRL_RedisCircuitFastFailAndRecovery(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	t.Cleanup(func() { chaos.StartRedisCluster(t); chaos.WaitRedisCluster(t, 60*time.Second) })
	chaos.StopRedisCluster(t)

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
