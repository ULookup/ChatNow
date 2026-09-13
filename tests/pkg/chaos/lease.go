package chaos

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
)

const identityRegistryKey = "/service/identity_service/instance"

// IdentityRegistered reads only the exact synthetic-stack registration key.
func IdentityRegistered(cfg *client.Config) (bool, error) {
	out, err := dockerAddress(cfg, "compose", "exec", "-T", "etcd", "etcdctl", "get", identityRegistryKey, "--write-out=json")
	if err != nil {
		return false, err
	}
	var response struct {
		Count int64 `json:"count"`
	}
	if err := json.Unmarshal(out, &response); err != nil {
		return false, fmt.Errorf("decode scoped registration: %w", err)
	}
	return response.Count == 1, nil
}

// ExpireIdentityLease suspends Identity until its registration expires, then
// resumes the same process. Cleanup repairs a failed baseline by restarting only
// Identity after the test's process-preservation assertion has finished.
func ExpireIdentityLease(t testing.TB, cfg *client.Config) func() {
	t.Helper()
	services := []string{"identity_server", "gateway_server", "transmite_server"}
	before := make([]containerAddress, len(services))
	for i, service := range services {
		out, err := dockerAddress(cfg, "compose", "ps", "-q", service)
		if err != nil || len(strings.Fields(string(out))) != 1 {
			t.Fatalf("expected one running %s container: %v", service, err)
		}
		before[i], err = inspectAddress(cfg, strings.TrimSpace(string(out)))
		if err != nil || !before[i].Running || before[i].Project == "" || before[i].Project != before[0].Project {
			t.Fatalf("lease fault requires one isolated Compose project: %v", err)
		}
	}
	registered, err := IdentityRegistered(cfg)
	if err != nil || !registered {
		t.Fatalf("Identity must be registered before lease fault: %v", err)
	}
	paused := true
	resume := func() {
		if paused {
			compose(t, cfg, "unpause", "identity_server")
			paused = false
		}
	}
	t.Cleanup(func() {
		resume()
		registered, err := IdentityRegistered(cfg)
		if err != nil || !registered {
			compose(t, cfg, "restart", "identity_server")
		}
	})
	compose(t, cfg, "pause", "identity_server")
	deadline := time.Now().Add(45 * time.Second)
	for time.Now().Before(deadline) {
		registered, err = IdentityRegistered(cfg)
		if err != nil {
			t.Fatal(err)
		}
		if !registered {
			resume()
			return func() {
				for i, service := range services {
					after, err := inspectAddress(cfg, before[i].ID)
					if err != nil || !after.Running || after.StartedAt != before[i].StartedAt {
						t.Fatalf("lease recovery restarted %s: %v", service, err)
					}
				}
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatal("Identity registration did not expire while renewal was suspended")
	return nil
}
