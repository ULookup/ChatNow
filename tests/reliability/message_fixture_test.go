//go:build reliability

package reliability_test

import (
	"context"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
)

// RL-MESSAGE-02 | P0 | Text fixtures wait for durable messages, not broker acceptance.
func TestRL_TextFixtureWaitsForPersistence(t *testing.T) {
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	t.Cleanup(func() { cleanup.CleanupAll(t, HTTP.Config()) })
	sender, _, conversation := fixture.MakeFriends(t, HTTP)
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	t.Cleanup(db.Close)
	restore := chaos.StopMessage(t, HTTP.Config())
	finished := make(chan struct{})
	t.Cleanup(func() {
		restore()
		select {
		case <-finished:
		case <-time.After(15 * time.Second):
			t.Error("message fixture did not finish after consumer restoration")
		}
		require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	})
	go func() {
		defer close(finished)
		id, seq := fixture.SendTextMessage(t, sender, conversation, "fixture-persistence")
		if id == 0 || seq == 0 {
			t.Error("message fixture returned incomplete identity")
			return
		}
		db.MessageExists(t, id)
	}()

	// The exact synthetic sender's accepted guard proves publication happened
	// while its consumer was stopped. No token or message body is inspected.
	var acceptedID int64
	deadline := time.Now().Add(5 * time.Second)
	for acceptedID == 0 && time.Now().Before(deadline) {
		for node := 1; node <= 6; node++ {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			output, err := exec.CommandContext(ctx, "docker", "exec", fmt.Sprintf("redis-node%d", node),
				"redis-cli", "-p", strconv.Itoa(6378+node), "--scan", "--pattern", "im:msg:idem:"+sender.UserID+":*").Output()
			cancel()
			require.NoError(t, err, "inspect synthetic sender's publication guard")
			for _, key := range strings.Fields(string(output)) {
				value := verify.RedisCLI(t, "GET", key)
				if raw, found := strings.CutPrefix(value, "accepted:"); found {
					acceptedID, err = strconv.ParseInt(raw, 10, 64)
					require.NoError(t, err)
				}
			}
		}
		if acceptedID == 0 {
			time.Sleep(25 * time.Millisecond)
		}
	}
	require.NotZero(t, acceptedID, "broker acceptance was not observed during the fault")
	db.MessageCount(t, conversation, 0)
	t.Cleanup(func() {
		restore()
		db.WaitMessageExists(t, acceptedID, 10*time.Second)
	})
	// This is a bounded negative observation under an established fault, not
	// a readiness delay. The positive assertion below checks actual MySQL state.
	select {
	case <-finished:
		t.Error("message fixture returned before the stopped consumer could persist")
	case <-time.After(250 * time.Millisecond):
	}
	restore()
	select {
	case <-finished:
	case <-time.After(15 * time.Second):
		t.Fatal("message fixture did not converge after restoring the consumer")
	}
	db.MessageExists(t, acceptedID)
}
