package chaos

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
)

// WorkerExit records only process diagnostics from the selected test container.
type WorkerExit struct {
	Running, OOMKilled    bool
	ExitCode              int
	StartedAt, FinishedAt string
	ProtectionLogged      bool
}

// RevokeWorkerLease revokes the sole worker lease in an isolated Compose stack.
// It temporarily disables restart so the original exit status remains observable.
// Cleanup restores the exact restart policy and starts Transmite even on RED.
func RevokeWorkerLease(t testing.TB, cfg *client.Config) (func() (WorkerExit, error), func()) {
	t.Helper()
	var containers []containerAddress
	for _, service := range []string{"transmite_server", "etcd"} {
		out, err := dockerAddress(cfg, "compose", "ps", "-q", service)
		if err != nil || len(strings.Fields(string(out))) != 1 {
			t.Fatalf("expected one running %s: %v", service, err)
		}
		state, err := inspectAddress(cfg, strings.TrimSpace(string(out)))
		if err != nil || !state.Running || state.Project == "" {
			t.Fatalf("inspect %s: %v", service, err)
		}
		containers = append(containers, state)
	}
	if containers[0].Project != containers[1].Project {
		t.Fatal("worker fault requires one isolated Compose project")
	}
	out, err := dockerAddress(cfg, "compose", "exec", "-T", "etcd", "etcdctl", "get", "/chatnow/snowflake/worker/", "--prefix", "--write-out=json")
	var leases struct {
		KVs []struct {
			Lease int64 `json:"lease"`
		} `json:"kvs"`
	}
	if err != nil || json.Unmarshal(out, &leases) != nil || len(leases.KVs) != 1 || leases.KVs[0].Lease == 0 {
		t.Fatalf("expected exactly one leased worker slot: %v", err)
	}
	id := containers[0].ID
	out, err = dockerAddress(cfg, "inspect", "--format", "{{json .HostConfig.RestartPolicy}}", id)
	var policy struct {
		Name              string
		MaximumRetryCount int
	}
	if err != nil || json.Unmarshal(out, &policy) != nil || policy.Name == "" {
		t.Fatalf("read worker restart policy: %v", err)
	}
	restart := policy.Name
	if restart == "on-failure" && policy.MaximumRetryCount > 0 {
		restart += ":" + strconv.Itoa(policy.MaximumRetryCount)
	}
	restored := false
	restore := func() {
		if restored {
			return
		}
		if _, e := dockerAddress(cfg, "update", "--restart="+restart, id); e != nil {
			t.Errorf("restore worker restart policy: %v", e)
			return
		}
		if _, e := dockerAddress(cfg, "start", id); e != nil {
			t.Errorf("restore worker process: %v", e)
			return
		}
		restored = true
	}
	t.Cleanup(restore)
	if _, err := dockerAddress(cfg, "update", "--restart=no", id); err != nil {
		t.Fatal(err)
	}
	lease := strconv.FormatInt(leases.KVs[0].Lease, 16)
	if _, err := dockerAddress(cfg, "compose", "exec", "-T", "etcd", "etcdctl", "lease", "revoke", lease); err != nil {
		t.Fatal(err)
	}
	t.Logf("revoked worker lease=%s original_start=%s", lease, containers[0].StartedAt)
	return func() (WorkerExit, error) {
		out, err := dockerAddress(cfg, "inspect", "--format", `{"Running":{{.State.Running}},"OOMKilled":{{.State.OOMKilled}},"ExitCode":{{.State.ExitCode}},"StartedAt":{{json .State.StartedAt}},"FinishedAt":{{json .State.FinishedAt}}}`, id)
		var state WorkerExit
		if err == nil {
			err = json.Unmarshal(out, &state)
		}
		if err == nil && state.StartedAt != containers[0].StartedAt {
			err = fmt.Errorf("worker restarted before its exit was recorded")
		}
		if err == nil && !state.Running {
			// Keep raw logs in memory; only expose the fixed diagnostic's presence.
			logs, logErr := dockerAddress(cfg, "logs", "--since", state.StartedAt, "--tail", "100", id)
			if logErr != nil {
				return state, fmt.Errorf("read worker exit diagnostic")
			}
			state.ProtectionLogged = strings.Contains(string(logs), "worker_lease_lost action=exit code=1")
		}
		return state, err
	}, restore
}

// ObserveWorkerRecovery rejects a delayed second exit after initial readiness.
// Seven seconds cover the five one-second TTL probes and the watchdog interval.
func ObserveWorkerRecovery(t testing.TB, cfg *client.Config) {
	t.Helper()
	out, err := dockerAddress(cfg, "compose", "ps", "-q", "transmite_server")
	if err != nil || len(strings.Fields(string(out))) != 1 {
		t.Fatalf("find recovered worker: %v", err)
	}
	id := strings.TrimSpace(string(out))
	before, err := inspectAddress(cfg, id)
	if err != nil || !before.Running {
		t.Fatalf("inspect recovered worker: %v", err)
	}
	deadline := time.Now().Add(7 * time.Second)
	for {
		after, err := inspectAddress(cfg, id)
		if err != nil || !after.Running || after.StartedAt != before.StartedAt {
			t.Fatalf("worker exited again after initial recovery: %v", err)
		}
		if time.Now().After(deadline) {
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
}

// WaitWorkerExit keeps observing the original process across watchdog cycles.
func WaitWorkerExit(t testing.TB, inspect func() (WorkerExit, error)) WorkerExit {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		state, err := inspect()
		if err != nil {
			t.Fatal(err)
		}
		if !state.Running {
			t.Logf("worker exit=%+v", state)
			return state
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatal("worker did not fence its process after confirmed lease loss")
	return WorkerExit{}
}
