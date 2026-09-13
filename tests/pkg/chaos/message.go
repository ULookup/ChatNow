package chaos

import (
	"testing"

	"chatnow-tests/pkg/client"
)

// StopMessage delays persistence and RPC lookup in a disposable Compose stack.
// The returned restore function is idempotent and also runs on assertion failure.
func StopMessage(t testing.TB, cfg *client.Config) func() {
	t.Helper()
	stopped := true
	restore := func() {
		if stopped {
			compose(t, cfg, "start", "message_server")
			stopped = false
		}
	}
	t.Cleanup(restore)
	compose(t, cfg, "stop", "--timeout", "5", "message_server")
	return restore
}
