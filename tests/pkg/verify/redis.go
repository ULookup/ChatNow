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
	out, err := RedisCLIResult(args...)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// RedisCLIResult runs redis-cli without invoking testing APIs, so callers may
// safely use it from polling callbacks and report failures on the test goroutine.
func RedisCLIResult(args ...string) (string, error) {
	base := []string{"exec", "redis-node1", "redis-cli", "-c"}
	out, err := exec.Command("docker", append(base, args...)...).CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("redis-cli %v: %w: %s", args, err, strings.TrimSpace(string(out)))
	}
	return strings.TrimSpace(string(out)), nil
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
	client := &http.Client{Timeout: 10 * time.Second}
	rsp, err := client.Get(fmt.Sprintf("%s/vars/%s?console=1", baseURL, name))
	if err != nil {
		t.Fatalf("read bvar %s: %v", name, err)
	}
	defer rsp.Body.Close()
	if rsp.StatusCode != http.StatusOK {
		t.Fatalf("read bvar %s: HTTP %d", name, rsp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(rsp.Body, 65536))
	if err != nil {
		t.Fatalf("read bvar body %s: %v", name, err)
	}
	text := strings.TrimSpace(string(body))
	if label, raw, found := strings.Cut(text, ":"); found {
		if strings.TrimSpace(label) != name {
			t.Fatalf("unexpected bvar label for %s", name)
		}
		text = strings.TrimSpace(raw)
	}
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil {
		t.Fatalf("bvar %s did not return an integer", name)
	}
	return value
}
