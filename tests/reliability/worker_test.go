//go:build reliability

package reliability_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/cleanup"
)

// RL-WORKER-01 | P0 | Worker lease loss exits explicitly, including as container PID 1.
func TestRL_WorkerLeaseLossFencesProcess(t *testing.T) {
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 20*time.Second))
	t.Cleanup(func() { require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 45*time.Second)) })
	inspect, restore := chaos.RevokeWorkerLease(t, HTTP.Config())
	exit := chaos.WaitWorkerExit(t, inspect)
	require.False(t, exit.OOMKilled, "worker protection must not be an OOM kill")
	require.Equal(t, 1, exit.ExitCode, "worker lease loss must exit explicitly, not through PID 1 signal fallback")
	require.True(t, exit.ProtectionLogged, "exit must identify worker ownership loss without sensitive data")
	restore()
	require.NoError(t, cleanup.WaitForStackReady(HTTP.Config(), 45*time.Second))
	chaos.ObserveWorkerRecovery(t, HTTP.Config())
}
