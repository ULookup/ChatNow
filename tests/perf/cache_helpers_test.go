//go:build perf

package perf_test

import (
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
)

func TestPF09PhaseConversationsRequireUniqueKeys(t *testing.T) {
	conversations := make([]pf09Conversation, pf09ConversationCount)
	for i := range conversations {
		conversations[i] = pf09Conversation{
			sender: &client.HTTPClient{UserID: "user-" + string(rune('a'+i))},
			id:     "conversation-" + string(rune('a'+i)),
		}
	}
	if err := validatePF09Conversations(conversations); err != nil {
		t.Fatalf("valid phase plan rejected: %v", err)
	}
	conversations[19].sender.UserID = conversations[0].sender.UserID
	if err := validatePF09Conversations(conversations); err == nil || !strings.Contains(err.Error(), "duplicate user-info key") {
		t.Fatalf("duplicate phase key not rejected: %v", err)
	}
}

func TestPF09EndpointsRejectEmptyDuplicateAndWrongCount(t *testing.T) {
	if _, err := parsePF09Endpoints("http://a,http://b", 2); err != nil {
		t.Fatalf("valid endpoints rejected: %v", err)
	}
	for _, test := range []struct {
		raw      string
		expected int
	}{
		{raw: "", expected: 1},
		{raw: "http://a,", expected: 1},
		{raw: "http://a/,http://a", expected: 2},
		{raw: "http://a", expected: 2},
	} {
		if _, err := parsePF09Endpoints(test.raw, test.expected); err == nil {
			t.Errorf("invalid endpoints accepted: raw=%q expected=%d", test.raw, test.expected)
		}
	}
}

func TestPF09SnapshotDeltaRejectsRestartRewindMissingAndOverflow(t *testing.T) {
	before := pf09Snapshot{
		"http://a": {pid: 10, uptimeSeconds: 100, counters: map[string]int64{
			pf09RPCMetric: 10, pf09L1Metric: 20, pf09L2Metric: 30,
		}},
	}
	after := pf09Snapshot{
		"http://a": {pid: 10, uptimeSeconds: 101, counters: map[string]int64{
			pf09RPCMetric: 11, pf09L1Metric: 22, pf09L2Metric: 33,
		}},
	}
	delta, err := pf09SnapshotDelta(before, after)
	if err != nil {
		t.Fatalf("valid snapshot rejected: %v", err)
	}
	if delta[pf09RPCMetric] != 1 || delta[pf09L1Metric] != 2 || delta[pf09L2Metric] != 3 {
		t.Fatalf("wrong deltas: %#v", delta)
	}

	cases := []pf09Snapshot{
		{},
		{"http://a": {pid: 11, uptimeSeconds: 1, counters: after["http://a"].counters}},
		{"http://a": {pid: 10, uptimeSeconds: 99, counters: after["http://a"].counters}},
		{"http://a": {pid: 10, uptimeSeconds: 101, counters: map[string]int64{
			pf09RPCMetric: 9, pf09L1Metric: 22, pf09L2Metric: 33,
		}}},
	}
	for i, invalid := range cases {
		if _, err := pf09SnapshotDelta(before, invalid); err == nil {
			t.Errorf("invalid snapshot %d accepted", i)
		}
	}

	overflowBefore := pf09Snapshot{
		"http://a": {pid: 1, uptimeSeconds: 1, counters: map[string]int64{pf09RPCMetric: 0, pf09L1Metric: 0, pf09L2Metric: 0}},
		"http://b": {pid: 2, uptimeSeconds: 1, counters: map[string]int64{pf09RPCMetric: 0, pf09L1Metric: 0, pf09L2Metric: 0}},
	}
	overflowAfter := pf09Snapshot{
		"http://a": {pid: 1, uptimeSeconds: 2, counters: map[string]int64{pf09RPCMetric: math.MaxInt64, pf09L1Metric: 0, pf09L2Metric: 0}},
		"http://b": {pid: 2, uptimeSeconds: 2, counters: map[string]int64{pf09RPCMetric: 1, pf09L1Metric: 0, pf09L2Metric: 0}},
	}
	if _, err := pf09SnapshotDelta(overflowBefore, overflowAfter); err == nil {
		t.Fatal("overflowing counter delta accepted")
	}
}

func TestPF09SnapshotRequiresProcessIdentityFields(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		values := map[string]string{
			"/vars/pid":                    "42",
			"/vars/user_info_rpc_total":    "1",
			"/vars/user_info_l1_hit_total": "2",
			"/vars/user_info_l2_hit_total": "3",
		}
		value, exists := values[r.URL.Path]
		if !exists {
			http.NotFound(w, r)
			return
		}
		_, _ = fmt.Fprint(w, value)
	}))
	defer server.Close()
	if _, err := capturePF09Snapshot([]string{server.URL}); err == nil || !strings.Contains(err.Error(), "process uptime") {
		t.Fatalf("missing process_uptime did not hard fail: %v", err)
	}
}

func TestPF09PhaseCountersProveStableCacheState(t *testing.T) {
	valid := []struct {
		state  pf09CacheState
		count  uint64
		deltas map[string]int64
	}{
		{pf09Cold, 20, map[string]int64{pf09RPCMetric: 20, pf09L1Metric: 0, pf09L2Metric: 0}},
		{pf09L2, 20, map[string]int64{pf09RPCMetric: 0, pf09L1Metric: 0, pf09L2Metric: 20}},
		{pf09L1, 20, map[string]int64{pf09RPCMetric: 0, pf09L1Metric: 20, pf09L2Metric: 0}},
		{pf09Stampede, 200, map[string]int64{pf09RPCMetric: 2, pf09L1Metric: 198, pf09L2Metric: 0}},
	}
	for _, test := range valid {
		if err := validatePF09PhaseCounters(test.state, test.count, 2, test.deltas); err != nil {
			t.Errorf("valid %s counters rejected: %v", pf09StateName(test.state), err)
		}
		contaminated := make(map[string]int64, len(test.deltas))
		for metric, value := range test.deltas {
			contaminated[metric] = value
		}
		contaminated[pf09RPCMetric]++
		if err := validatePF09PhaseCounters(test.state, test.count, 2, contaminated); err == nil {
			t.Errorf("contaminated %s counters accepted", pf09StateName(test.state))
		}
	}
}

func TestPF09GateDurationCannotBeBypassed(t *testing.T) {
	t.Setenv("PF09_GATE_MIN_DURATION", "24h")
	if pf09SteadyDuration != 10*time.Second {
		t.Fatalf("steady duration changed through environment: %s", pf09SteadyDuration)
	}
	if err := validatePF09IterationCount(1); err != nil {
		t.Fatalf("one-shot invocation rejected: %v", err)
	}
	if err := validatePF09IterationCount(2); err == nil {
		t.Fatal("calibrated invocation accepted; -benchtime=1x is mandatory")
	}
	if _, present := os.LookupEnv("PF09_GATE_MIN_DURATION"); !present {
		t.Fatal("test precondition lost")
	}
}
