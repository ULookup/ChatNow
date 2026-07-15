package chaos

import (
	"os/exec"
	"testing"
	"time"
)

var redisServices = []string{
	"redis-node1", "redis-node2", "redis-node3",
	"redis-node4", "redis-node5", "redis-node6",
}

func compose(t testing.TB, args ...string) {
	t.Helper()
	cmd := exec.Command("docker", append([]string{"compose"}, args...)...)
	cmd.Dir = ".."
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("docker compose %v: %v: %s", args, err, out)
	}
}

func StopRedisCluster(t testing.TB) {
	compose(t, append([]string{"stop"}, redisServices...)...)
}

func StartRedisCluster(t testing.TB) {
	compose(t, append([]string{"start"}, redisServices...)...)
}

func WaitRedisCluster(t testing.TB, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		cmd := exec.Command("docker", "exec", "redis-node1", "redis-cli", "cluster", "info")
		if out, err := cmd.Output(); err == nil && string(out) != "" {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("redis cluster did not recover within %s", timeout)
}
