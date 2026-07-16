//go:build perf

package perf_test

import (
	"bytes"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
	"google.golang.org/protobuf/proto"
)

const (
	pf09ConversationCount      = 20
	pf09MaxLatencySamples      = 65_536
	pf09MinThroughput          = 5_000.0
	pf09CheckedInBaseline      = 5_000.0
	pf09MinRPCReduction        = 95.0
	pf09MaxBaselineRegression  = 10.0
	pf09SteadyDuration         = 10 * time.Second
	pf09SnapshotTimeout        = 2 * time.Second
	pf09StartEstimateTolerance = 2 * time.Second

	pf09RPCMetric = "user_info_rpc_total"
	pf09L1Metric  = "user_info_l1_hit_total"
	pf09L2Metric  = "user_info_l2_hit_total"
)

var pf09CounterMetrics = [...]string{pf09RPCMetric, pf09L1Metric, pf09L2Metric}

type pf09CacheState uint8

const (
	pf09Cold pf09CacheState = iota
	pf09L2
	pf09L1
)

type pf09Conversation struct {
	sender *client.HTTPClient
	id     string
}

type pf09InstanceSnapshot struct {
	pid                   int64
	uptimeSeconds         float64
	startedAtEstimateNano int64
	counters              map[string]int64
}

type pf09Snapshot map[string]pf09InstanceSnapshot

type pf09PhaseResult struct {
	elapsed   time.Duration
	successes uint64
	latencies []int64
	err       error
}

// BenchmarkPF09_UserInfoCache is intentionally a one-shot benchmark harness.
// cold/L2 each execute one concurrent operation for 20 unique sender keys; L1
// executes a fixed ten-second steady workload over 20 prewarmed senders. This
// prevents Go's adaptive benchmark calibration from changing the cache state.
func BenchmarkPF09_UserInfoCache(b *testing.B) {
	if os.Getenv("PF09_RUN_FULLSTACK") != "1" {
		b.Skip("PF-09 requires the full service stack; use make test-perf-cache-gate on Linux")
	}
	if runtime.GOOS != "linux" {
		b.Fatalf("PF-09 gate is calibrated for Linux, got %s", runtime.GOOS)
	}
	if err := validatePF09IterationCount(b.N); err != nil {
		b.Fatal(err)
	}

	baseline := pf09Baseline(b)
	endpoints := pf09ConfiguredEndpoints(b)
	for _, phase := range []struct {
		name  string
		state pf09CacheState
	}{
		{name: "cold", state: pf09Cold},
		{name: "L2", state: pf09L2},
		{name: "L1", state: pf09L1},
	} {
		phase := phase
		b.Run(phase.name, func(b *testing.B) {
			if err := validatePF09IterationCount(b.N); err != nil {
				b.Fatal(err)
			}
			conversations := preparePF09Conversations(b, phase.state, len(endpoints))
			if err := validatePF09Conversations(conversations); err != nil {
				b.Fatalf("PF-09 %s phase plan: %v", phase.name, err)
			}
			runPF09Phase(b, phase.state, conversations, endpoints, baseline)
		})
	}
}

func validatePF09IterationCount(n int) error {
	if n != 1 {
		return fmt.Errorf("PF-09 requires -benchtime=1x to preserve phase state, got b.N=%d", n)
	}
	return nil
}

func preparePF09Conversations(b *testing.B, state pf09CacheState, instanceCount int) []pf09Conversation {
	b.Helper()
	b.StopTimer()
	conversations := make([]pf09Conversation, pf09ConversationCount)
	for i := range conversations {
		sender, peer, conversationID := fixture.MakeFriends(b, HTTP)
		_ = peer
		conversations[i] = pf09Conversation{sender: sender, id: conversationID}
		if state == pf09L2 || state == pf09L1 {
			seedPF09L2(b, sender)
		}
	}
	if state == pf09L1 {
		// Gateway dispatch is round-robin. Consecutive instanceCount sends per
		// sender warm that sender in every declared Transmite process.
		for i, conversation := range conversations {
			for instance := 0; instance < instanceCount; instance++ {
				sequence := uint64(i*instanceCount + instance)
				if err := sendPF09Message(conversation, sequence); err != nil {
					b.Fatalf("warm PF-09 L1: %v", err)
				}
			}
		}
	}
	return conversations
}

func validatePF09Conversations(conversations []pf09Conversation) error {
	if len(conversations) != pf09ConversationCount {
		return fmt.Errorf("want %d conversations, got %d", pf09ConversationCount, len(conversations))
	}
	keys := make(map[string]struct{}, len(conversations))
	conversationIDs := make(map[string]struct{}, len(conversations))
	for _, conversation := range conversations {
		if conversation.sender == nil || conversation.sender.UserID == "" || conversation.id == "" {
			return fmt.Errorf("empty sender or conversation")
		}
		key := pf09UserInfoKey(conversation.sender.UserID)
		if _, exists := keys[key]; exists {
			return fmt.Errorf("duplicate user-info key %q", key)
		}
		keys[key] = struct{}{}
		if _, exists := conversationIDs[conversation.id]; exists {
			return fmt.Errorf("duplicate conversation %q", conversation.id)
		}
		conversationIDs[conversation.id] = struct{}{}
	}
	return nil
}

func seedPF09L2(b *testing.B, sender *client.HTTPClient) {
	b.Helper()
	rsp := &identity.GetProfileRsp{}
	if err := sender.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(),
	}, rsp); err != nil {
		b.Fatalf("read PF-09 profile: %v", err)
	}
	if !rsp.GetHeader().GetSuccess() || rsp.GetUserInfo() == nil {
		b.Fatalf("read PF-09 profile: %s", rsp.GetHeader().GetErrorMessage())
	}
	value, err := proto.Marshal(rsp.GetUserInfo())
	if err != nil {
		b.Fatalf("marshal PF-09 profile: %v", err)
	}
	key := pf09UserInfoKey(sender.UserID)
	const seedScript = "return redis.call('SET',KEYS[1],ARGV[2],'EX',ARGV[1])"
	set := exec.Command(
		"docker", "exec", "-i", HTTP.Config().Infra.RedisContainer,
		"redis-cli", "-c", "-x", "EVAL", seedScript, "1", key, "3600",
	)
	set.Stdin = bytes.NewReader(value)
	if out, err := set.CombinedOutput(); err != nil || strings.TrimSpace(string(out)) != "OK" {
		b.Fatalf("seed PF-09 L2 %s: %v: %s", key, err, strings.TrimSpace(string(out)))
	}
}

func runPF09Phase(
	b *testing.B,
	state pf09CacheState,
	conversations []pf09Conversation,
	endpoints []string,
	baseline float64,
) {
	b.Helper()
	before, err := capturePF09Snapshot(endpoints)
	if err != nil {
		b.Fatalf("PF-09 before snapshot: %v", err)
	}

	b.ResetTimer()
	var result pf09PhaseResult
	if state == pf09L1 {
		result = runPF09Steady(conversations)
	} else {
		result = runPF09SingleUse(conversations)
	}
	b.StopTimer()
	if result.err != nil {
		b.Fatalf("PF-09 %s send failed: %v", pf09StateName(state), result.err)
	}
	if result.successes == 0 || result.elapsed <= 0 || len(result.latencies) == 0 {
		b.Fatalf("PF-09 %s produced no measurable successful requests", pf09StateName(state))
	}

	after, err := capturePF09Snapshot(endpoints)
	if err != nil {
		b.Fatalf("PF-09 after snapshot: %v", err)
	}
	deltas, err := pf09SnapshotDelta(before, after)
	if err != nil {
		b.Fatalf("PF-09 snapshot integrity: %v", err)
	}
	if err := validatePF09PhaseCounters(state, result.successes, deltas); err != nil {
		b.Error(err)
	}

	sort.Slice(result.latencies, func(i, j int) bool { return result.latencies[i] < result.latencies[j] })
	p95Index := int(math.Ceil(float64(len(result.latencies))*0.95)) - 1
	throughput := float64(result.successes) / result.elapsed.Seconds()
	p95Micros := float64(result.latencies[p95Index]) / float64(time.Microsecond)
	rpcReduction := 100 * (1 - float64(deltas[pf09RPCMetric])/float64(result.successes))
	baselineRegression := 100 * (1 - throughput/baseline)
	b.ReportMetric(throughput, "msg/s")
	b.ReportMetric(p95Micros, "p95-us")
	b.ReportMetric(rpcReduction, "rpc-reduction-%")
	b.ReportMetric(baselineRegression, "baseline-regression-%")

	if state == pf09L1 {
		if result.elapsed < pf09SteadyDuration {
			b.Errorf("PF-09 L1 duration %s is below fixed gate duration %s", result.elapsed, pf09SteadyDuration)
		}
		if throughput < pf09MinThroughput {
			b.Errorf("PF-09 L1 throughput %.2f msg/s is below %.2f msg/s", throughput, pf09MinThroughput)
		}
		if baselineRegression > pf09MaxBaselineRegression {
			b.Errorf("PF-09 L1 throughput regressed %.2f%% from baseline %.2f msg/s", baselineRegression, baseline)
		}
	}
}

func runPF09SingleUse(conversations []pf09Conversation) pf09PhaseResult {
	start := make(chan struct{})
	results := make(chan struct {
		latency int64
		err     error
	}, len(conversations))
	var wg sync.WaitGroup
	for i, conversation := range conversations {
		i, conversation := i, conversation
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			requestStarted := time.Now()
			err := sendPF09Message(conversation, uint64(i))
			results <- struct {
				latency int64
				err     error
			}{time.Since(requestStarted).Nanoseconds(), err}
		}()
	}
	started := time.Now()
	close(start)
	wg.Wait()
	elapsed := time.Since(started)
	close(results)
	result := pf09PhaseResult{elapsed: elapsed, latencies: make([]int64, 0, len(conversations))}
	for item := range results {
		if item.err != nil && result.err == nil {
			result.err = item.err
		}
		if item.err == nil {
			result.successes++
			result.latencies = append(result.latencies, item.latency)
		}
	}
	return result
}

func runPF09Steady(conversations []pf09Conversation) pf09PhaseResult {
	workers := max(pf09ConversationCount, runtime.GOMAXPROCS(0)*4)
	perWorkerSamples := max(1, pf09MaxLatencySamples/workers)
	start := make(chan struct{})
	deadline := time.Time{}
	var sequence atomic.Uint64
	var successes atomic.Uint64
	var stopped atomic.Bool
	var firstError error
	var errorMu sync.Mutex
	latencies := make([]int64, 0, pf09MaxLatencySamples)
	var latencyMu sync.Mutex
	var wg sync.WaitGroup
	for worker := 0; worker < workers; worker++ {
		worker := worker
		wg.Add(1)
		go func() {
			defer wg.Done()
			local := make([]int64, 0, perWorkerSamples)
			var seen uint64
			randomState := uint64(worker+1) | 1
			<-start
			for !stopped.Load() && time.Now().Before(deadline) {
				id := sequence.Add(1) - 1
				conversation := conversations[id%uint64(len(conversations))]
				requestStarted := time.Now()
				err := sendPF09Message(conversation, id)
				latency := time.Since(requestStarted).Nanoseconds()
				if err != nil {
					errorMu.Lock()
					if firstError == nil {
						firstError = err
						stopped.Store(true)
					}
					errorMu.Unlock()
					break
				}
				successes.Add(1)
				seen++
				local, randomState = pf09ReservoirAdd(local, perWorkerSamples, seen, randomState, latency)
			}
			latencyMu.Lock()
			remaining := pf09MaxLatencySamples - len(latencies)
			if len(local) > remaining {
				local = local[:remaining]
			}
			latencies = append(latencies, local...)
			latencyMu.Unlock()
		}()
	}
	started := time.Now()
	deadline = started.Add(pf09SteadyDuration)
	close(start)
	wg.Wait()
	return pf09PhaseResult{
		elapsed:   time.Since(started),
		successes: successes.Load(),
		latencies: latencies,
		err:       firstError,
	}
}

func pf09ReservoirAdd(samples []int64, capacity int, seen, state uint64, value int64) ([]int64, uint64) {
	if len(samples) < capacity {
		return append(samples, value), state
	}
	state ^= state << 13
	state ^= state >> 7
	state ^= state << 17
	if replacement := state % seen; replacement < uint64(len(samples)) {
		samples[replacement] = value
	}
	return samples, state
}

func validatePF09PhaseCounters(state pf09CacheState, successes uint64, deltas map[string]int64) error {
	if successes > math.MaxInt64 {
		return fmt.Errorf("PF-09 success count overflows int64")
	}
	want := int64(successes)
	switch state {
	case pf09Cold:
		if deltas[pf09RPCMetric] != want || deltas[pf09L1Metric] != 0 || deltas[pf09L2Metric] != 0 {
			return fmt.Errorf("PF-09 cold state contaminated: success=%d rpc=%d l1=%d l2=%d", want, deltas[pf09RPCMetric], deltas[pf09L1Metric], deltas[pf09L2Metric])
		}
	case pf09L2:
		if deltas[pf09L2Metric] != want || deltas[pf09L1Metric] != 0 || deltas[pf09RPCMetric] != 0 {
			return fmt.Errorf("PF-09 L2 state contaminated: success=%d rpc=%d l1=%d l2=%d", want, deltas[pf09RPCMetric], deltas[pf09L1Metric], deltas[pf09L2Metric])
		}
	case pf09L1:
		if deltas[pf09L1Metric] != want || deltas[pf09L2Metric] != 0 || deltas[pf09RPCMetric] != 0 {
			return fmt.Errorf("PF-09 L1 state contaminated: success=%d rpc=%d l1=%d l2=%d", want, deltas[pf09RPCMetric], deltas[pf09L1Metric], deltas[pf09L2Metric])
		}
		if 100*(1-float64(deltas[pf09RPCMetric])/float64(want)) < pf09MinRPCReduction {
			return fmt.Errorf("PF-09 L1 RPC reduction below %.0f%%", pf09MinRPCReduction)
		}
	default:
		return fmt.Errorf("unknown PF-09 state %d", state)
	}
	return nil
}

func pf09ConfiguredEndpoints(b *testing.B) []string {
	b.Helper()
	raw := os.Getenv("PF09_TRANSMITE_VARS_URLS")
	if raw == "" {
		raw = HTTP.Config().Infra.TransmiteVars
	}
	value := os.Getenv("PF09_EXPECTED_TRANSMITE_INSTANCES")
	if value == "" {
		b.Fatal("PF09_EXPECTED_TRANSMITE_INSTANCES is required by the full-stack gate")
	}
	expected, err := strconv.Atoi(value)
	if err != nil || expected <= 0 {
		b.Fatalf("PF09_EXPECTED_TRANSMITE_INSTANCES must be positive, got %q", value)
	}
	endpoints, err := parsePF09Endpoints(raw, expected)
	if err != nil {
		b.Fatal(err)
	}
	return endpoints
}

func parsePF09Endpoints(raw string, expected int) ([]string, error) {
	parts := strings.Split(raw, ",")
	endpoints := make([]string, 0, len(parts))
	seen := make(map[string]struct{}, len(parts))
	for _, part := range parts {
		endpoint := strings.TrimRight(strings.TrimSpace(part), "/")
		if endpoint == "" {
			return nil, fmt.Errorf("PF-09 contains an empty Transmite bvar endpoint")
		}
		if _, duplicate := seen[endpoint]; duplicate {
			return nil, fmt.Errorf("PF-09 duplicate Transmite bvar endpoint %q", endpoint)
		}
		seen[endpoint] = struct{}{}
		endpoints = append(endpoints, endpoint)
	}
	if len(endpoints) != expected {
		return nil, fmt.Errorf("PF-09 expected %d Transmite instances, got %d", expected, len(endpoints))
	}
	return endpoints, nil
}

func capturePF09Snapshot(endpoints []string) (pf09Snapshot, error) {
	snapshot := make(pf09Snapshot, len(endpoints))
	for _, endpoint := range endpoints {
		pid, err := readPF09Int(endpoint, "pid")
		if err != nil || pid <= 0 {
			return nil, fmt.Errorf("%s process identity: pid=%d err=%w", endpoint, pid, err)
		}
		uptime, err := readPF09Float(endpoint, "process_uptime")
		if err != nil || uptime < 0 {
			return nil, fmt.Errorf("%s process uptime: uptime=%f err=%w", endpoint, uptime, err)
		}
		instance := pf09InstanceSnapshot{
			pid:                   pid,
			uptimeSeconds:         uptime,
			startedAtEstimateNano: time.Now().Add(-time.Duration(uptime * float64(time.Second))).UnixNano(),
			counters:              make(map[string]int64, len(pf09CounterMetrics)),
		}
		for _, metric := range pf09CounterMetrics {
			value, err := readPF09Int(endpoint, metric)
			if err != nil || value < 0 {
				return nil, fmt.Errorf("%s %s: value=%d err=%w", endpoint, metric, value, err)
			}
			instance.counters[metric] = value
		}
		snapshot[endpoint] = instance
	}
	return snapshot, nil
}

func readPF09Int(endpoint, name string) (int64, error) {
	raw, err := readPF09BVar(endpoint, name)
	if err != nil {
		return 0, err
	}
	return strconv.ParseInt(strings.Trim(raw, "\""), 10, 64)
}

func readPF09Float(endpoint, name string) (float64, error) {
	raw, err := readPF09BVar(endpoint, name)
	if err != nil {
		return 0, err
	}
	value, err := strconv.ParseFloat(strings.Trim(raw, "\""), 64)
	if err != nil || math.IsNaN(value) || math.IsInf(value, 0) {
		return 0, fmt.Errorf("parse %s=%q", name, raw)
	}
	return value, nil
}

func readPF09BVar(endpoint, name string) (string, error) {
	client := &http.Client{Timeout: pf09SnapshotTimeout}
	rsp, err := client.Get(endpoint + "/vars/" + name)
	if err != nil {
		return "", err
	}
	defer rsp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(rsp.Body, 4096))
	if err != nil {
		return "", err
	}
	if rsp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("HTTP %d: %s", rsp.StatusCode, strings.TrimSpace(string(body)))
	}
	value := strings.TrimSpace(string(body))
	if value == "" {
		return "", fmt.Errorf("empty bvar %s", name)
	}
	return value, nil
}

func pf09SnapshotDelta(before, after pf09Snapshot) (map[string]int64, error) {
	if len(before) == 0 || len(before) != len(after) {
		return nil, fmt.Errorf("snapshot endpoint set changed: before=%d after=%d", len(before), len(after))
	}
	totals := make(map[string]int64, len(pf09CounterMetrics))
	for endpoint, old := range before {
		current, exists := after[endpoint]
		if !exists {
			return nil, fmt.Errorf("snapshot endpoint %s disappeared", endpoint)
		}
		if old.pid != current.pid {
			return nil, fmt.Errorf("snapshot endpoint %s restarted: pid %d -> %d", endpoint, old.pid, current.pid)
		}
		if current.uptimeSeconds < old.uptimeSeconds {
			return nil, fmt.Errorf("snapshot endpoint %s uptime rewound: %f -> %f", endpoint, old.uptimeSeconds, current.uptimeSeconds)
		}
		startDrift := time.Duration(current.startedAtEstimateNano - old.startedAtEstimateNano)
		if startDrift < -pf09StartEstimateTolerance || startDrift > pf09StartEstimateTolerance {
			return nil, fmt.Errorf("snapshot endpoint %s process start estimate changed by %s", endpoint, startDrift)
		}
		for _, metric := range pf09CounterMetrics {
			oldValue, oldOK := old.counters[metric]
			newValue, newOK := current.counters[metric]
			if !oldOK || !newOK {
				return nil, fmt.Errorf("snapshot endpoint %s missing %s", endpoint, metric)
			}
			if newValue < oldValue {
				return nil, fmt.Errorf("snapshot endpoint %s counter %s rewound: %d -> %d", endpoint, metric, oldValue, newValue)
			}
			delta := newValue - oldValue
			if delta > math.MaxInt64-totals[metric] {
				return nil, fmt.Errorf("snapshot counter %s delta overflow", metric)
			}
			totals[metric] += delta
		}
	}
	return totals, nil
}

func sendPF09Message(conversation pf09Conversation, sequence uint64) error {
	rsp := &transmite.SendMessageRsp{}
	err := conversation.sender.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: conversation.id,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("pf09-%d", sequence)}},
		},
		ClientMsgId: client.NewRequestID(),
	}, rsp)
	if err != nil {
		return err
	}
	if !rsp.GetHeader().GetSuccess() {
		return fmt.Errorf("code=%d message=%s", rsp.GetHeader().GetErrorCode(), rsp.GetHeader().GetErrorMessage())
	}
	return nil
}

func pf09Baseline(b *testing.B) float64 {
	b.Helper()
	value := pf09CheckedInBaseline
	if raw := os.Getenv("PF09_BASELINE_MSG_PER_SEC"); raw != "" {
		parsed, err := strconv.ParseFloat(raw, 64)
		if err != nil || math.IsNaN(parsed) || math.IsInf(parsed, 0) || parsed < pf09CheckedInBaseline {
			b.Fatalf("PF09_BASELINE_MSG_PER_SEC must be finite and >= %.0f, got %q", pf09CheckedInBaseline, raw)
		}
		value = parsed
	}
	return value
}

func pf09UserInfoKey(uid string) string {
	const offset32 = uint32(2166136261)
	const prime32 = uint32(16777619)
	hash := offset32
	for i := 0; i < len(uid); i++ {
		hash ^= uint32(uid[i])
		hash *= prime32
	}
	return fmt.Sprintf("im:user:{%d}:%s", hash%64, uid)
}

func pf09StateName(state pf09CacheState) string {
	switch state {
	case pf09Cold:
		return "cold"
	case pf09L2:
		return "L2"
	case pf09L1:
		return "L1"
	default:
		return "unknown"
	}
}
