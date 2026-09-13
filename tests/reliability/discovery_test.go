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
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// RL-DISCOVERY-01 | P1 | RPC callers recover after a service hostname changes IP.
func TestRL_DiscoveryRecoversAfterIdentityAddressChange(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	peer, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, user, []*client.HTTPClient{peer}, "rl-discovery")
	profileWorks := func() bool {
		rsp := &identity.GetProfileRsp{}
		err := user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{RequestId: client.NewRequestID()}, rsp)
		return err == nil && rsp.GetHeader().GetSuccess()
	}
	require.True(t, profileWorks(), "Identity must work before the address fault")
	// Registered before the fault controller so this runs after network cleanup.
	// A restored interface alone does not prove the next suite can call Identity.
	t.Cleanup(func() {
		require.Eventually(t, profileWorks, 45*time.Second, time.Second,
			"account RPC must recover after restoring the original Identity endpoint")
	})
	checkProcesses := chaos.ChangeIdentityAddress(t, HTTP.Config())
	started := time.Now()
	// This user has not sent before the fault, so Transmite must fetch Identity
	// profile data instead of succeeding from a prewarmed user-info cache.
	require.Eventually(t, func() bool {
		if !profileWorks() {
			return false
		}
		rsp := &transmite.SendMessageRsp{}
		err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
			RequestId: client.NewRequestID(), ConversationId: convID,
			Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "after-address-change"}}},
			ClientMsgId: client.NewRequestID(),
		}, rsp)
		return err == nil && rsp.GetHeader().GetSuccess()
	}, 45*time.Second, time.Second,
		"Gateway and Transmite must resolve the new Identity address without a caller restart")
	checkProcesses()
	t.Logf("account and message RPCs recovered in %s", time.Since(started))
}
