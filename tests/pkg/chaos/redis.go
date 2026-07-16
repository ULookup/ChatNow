package chaos

import (
	"os/exec"
	"strings"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
)

var redisServices = []string{
	"redis-node1", "redis-node2", "redis-node3",
	"redis-node4", "redis-node5", "redis-node6",
}

func compose(t testing.TB, cfg *client.Config, args ...string) {
	t.Helper()
	cmd := exec.Command("docker", append([]string{"compose"}, args...)...)
	cmd.Dir = cfg.Infra.ComposeDir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("docker compose %v: %v: %s", args, err, out)
	}
}

func StopRedisCluster(t testing.TB, cfg *client.Config) {
	// Fail all AOF-backed test nodes immediately so short-lived service L1
	// entries remain warm for deterministic failover tests.
	compose(t, cfg, append([]string{"stop", "-t", "0"}, redisServices...)...)
}

func StartRedisCluster(t testing.TB, cfg *client.Config) {
	compose(t, cfg, append([]string{"start"}, redisServices...)...)
}

func WaitRedisCluster(t testing.TB, cfg *client.Config, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		cmd := exec.Command("docker", "exec", cfg.Infra.RedisContainer, "redis-cli", "cluster", "info")
		if out, err := cmd.Output(); err == nil && clusterHealthy(string(out)) {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("redis cluster did not recover within %s", timeout)
}

func clusterHealthy(info string) bool {
	values := make(map[string]string)
	for _, line := range strings.Split(strings.ReplaceAll(info, "\r", ""), "\n") {
		parts := strings.SplitN(line, ":", 2)
		if len(parts) == 2 {
			values[strings.TrimSpace(parts[0])] = strings.TrimSpace(parts[1])
		}
	}
	return values["cluster_state"] == "ok" &&
		values["cluster_slots_assigned"] == "16384" &&
		values["cluster_slots_ok"] == "16384" &&
		values["cluster_slots_fail"] == "0"
}
