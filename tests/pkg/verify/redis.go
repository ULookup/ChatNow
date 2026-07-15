package verify

import (
	"fmt"
	"io"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"
)

func RedisCLI(t testing.TB, args ...string) string {
	t.Helper()
	base := []string{"exec", "redis-node1", "redis-cli", "-c"}
	out, err := exec.Command("docker", append(base, args...)...).CombinedOutput()
	if err != nil {
		t.Fatalf("redis-cli %v: %v: %s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func RedisTTL(t testing.TB, key string) time.Duration {
	seconds, err := strconv.ParseInt(RedisCLI(t, "TTL", key), 10, 64)
	if err != nil || seconds < 0 {
		t.Fatalf("invalid TTL for %s: %d (%v)", key, seconds, err)
	}
	return time.Duration(seconds) * time.Second
}

func BVar(t testing.TB, baseURL, name string) int64 {
	t.Helper()
	rsp, err := http.Get(fmt.Sprintf("%s/vars/%s", baseURL, name))
	if err != nil {
		t.Fatalf("read bvar %s: %v", name, err)
	}
	defer rsp.Body.Close()
	body, err := io.ReadAll(rsp.Body)
	if err != nil {
		t.Fatalf("read bvar body %s: %v", name, err)
	}
	value, err := strconv.ParseInt(strings.TrimSpace(string(body)), 10, 64)
	if err != nil {
		t.Fatalf("parse bvar %s=%q: %v", name, body, err)
	}
	return value
}
