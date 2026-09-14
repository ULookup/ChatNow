//go:build reliability

package reliability_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// RL-DISCOVERY-02 | P0 | Expired registration recovers without process restarts.
func TestRL_RegistryRecoversAfterLeaseExpiry(t *testing.T) {
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	t.Cleanup(func() { cleanup.CleanupAll(t, HTTP.Config()) })
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	profileWorks := func() bool {
		rsp := &identity.GetProfileRsp{}
		return user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{RequestId: client.NewRequestID()}, rsp) == nil && rsp.GetHeader().GetSuccess()
	}
	require.True(t, profileWorks(), "profile control must succeed before the fault")
	t.Cleanup(func() {
		require.Eventually(t, profileWorks, 30*time.Second, 200*time.Millisecond, "fault cleanup must restore Identity RPC")
	})
	checkProcesses := chaos.ExpireIdentityLease(t, HTTP.Config())
	started := time.Now()
	require.Eventually(t, func() bool {
		registered, err := chaos.IdentityRegistered(HTTP.Config())
		return err == nil && registered && profileWorks()
	}, 60*time.Second, 500*time.Millisecond, "expired lease must recover registration and account RPC without restarting")
	checkProcesses()
	t.Logf("registration and account RPC recovered in %s", time.Since(started))
}
