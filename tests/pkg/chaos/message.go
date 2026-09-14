package chaos

import (
	"testing"

	"chatnow-tests/pkg/client"
)

// StopMessage delays persistence and RPC lookup in a disposable Compose stack.
// The returned restore function is idempotent and also runs on assertion failure.
func StopMessage(t testing.TB, cfg *client.Config) func() {
	return stopDeliveryService(t, cfg, "message_server")
}

// StopPush interrupts the ACK owner while preserving Redis delivery state.
func StopPush(t testing.TB, cfg *client.Config) func() {
	return stopDeliveryService(t, cfg, "push_server")
}

func stopDeliveryService(t testing.TB, cfg *client.Config, service string) func() {
	t.Helper()
	stopped := true
	restore := func() {
		if stopped {
			compose(t, cfg, "start", service)
			stopped = false
		}
	}
	t.Cleanup(restore)
	compose(t, cfg, "stop", "--timeout", "5", service)
	return restore
}
