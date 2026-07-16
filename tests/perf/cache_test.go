//go:build perf

package perf_test

import (
	"bytes"
	"fmt"
	"math"
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
	"chatnow-tests/pkg/verify"
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
	pf09DefaultGateMinDuration = 5 * time.Second
)

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

// BenchmarkPF09_UserInfoCache is a full-stack benchmark. It deliberately skips
// unless PF09_RUN_FULLSTACK=1, so developer compile checks never masquerade as a
// performance result. The gate target in tests/Makefile enables it on Linux CI.
func BenchmarkPF09_UserInfoCache(b *testing.B) {
	if os.Getenv("PF09_RUN_FULLSTACK") != "1" {
		b.Skip("PF-09 requires the full service stack; use make test-perf-cache-gate on Linux")
	}
	if runtime.GOOS != "linux" {
		b.Fatalf("PF-09 gate is calibrated for Linux, got %s", runtime.GOOS)
	}

	baseline := pf09Baseline(b)
	gateMinDuration := pf09GateMinDuration(b)
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
			conversations := preparePF09Conversations(b, phase.state)
			runPF09Phase(b, phase.state, conversations, baseline, gateMinDuration)
		})
	}
}

func preparePF09Conversations(b *testing.B, state pf09CacheState) []pf09Conversation {
	b.Helper()
	b.StopTimer()

	conversations := make([]pf09Conversation, pf09ConversationCount)
	for i := range conversations {
		sender, peer, conversationID := fixture.MakeFriends(b, HTTP)
		_ = peer
		conversations[i] = pf09Conversation{sender: sender, id: conversationID}

		switch state {
		case pf09Cold:
			// A newly registered sender has neither L1 nor L2 cache state.
		case pf09L2, pf09L1:
			seedPF09L2(b, sender)
		default:
			b.Fatalf("unknown PF-09 cache state %d", state)
		}
	}

	if state == pf09L1 {
		for i, conversation := range conversations {
			if err := sendPF09Message(conversation, uint64(i)); err != nil {
				b.Fatalf("warm PF-09 L1: %v", err)
			}
		}
	}
	return conversations
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
	container := HTTP.Config().Infra.RedisContainer
	const seedScript = "return redis.call('SET',KEYS[1],ARGV[2],'EX',ARGV[1])"
	set := exec.Command(
		"docker", "exec", "-i", container, "redis-cli", "-c", "-x",
		"EVAL", seedScript, "1", key, "3600",
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
	baseline float64,
	gateMinDuration time.Duration,
) {
	b.Helper()
	beforeRPC := pf09RPCSnapshot(b)

	var sequence atomic.Uint64
	var successes atomic.Uint64
	var workerSeeds atomic.Uint64
	var stopped atomic.Bool
	var failureMu sync.Mutex
	var firstFailure error
	latencies := make([]int64, 0, pf09MaxLatencySamples)
	var latencyMu sync.Mutex
	perWorkerSamples := max(1, pf09MaxLatencySamples/max(1, runtime.GOMAXPROCS(0)))

	b.ResetTimer()
	started := time.Now()
	b.RunParallel(func(pb *testing.PB) {
		localLatencies := make([]int64, 0, perWorkerSamples)
		var samplesSeen uint64
		randomState := (uint64(time.Now().UnixNano()) ^ workerSeeds.Add(1)) | 1
		for pb.Next() {
			if stopped.Load() {
				break
			}
			id := sequence.Add(1) - 1
			conversation := conversations[id%uint64(len(conversations))]
			requestStarted := time.Now()
			err := sendPF09Message(conversation, id)
			latency := time.Since(requestStarted).Nanoseconds()
			if err != nil {
				failureMu.Lock()
				if firstFailure == nil {
					firstFailure = err
					stopped.Store(true)
				}
				failureMu.Unlock()
				break
			}
			successes.Add(1)
			samplesSeen++
			if len(localLatencies) < cap(localLatencies) {
				localLatencies = append(localLatencies, latency)
			} else {
				// Per-worker reservoir sampling keeps p95 memory bounded without
				// biasing the sample toward benchmark startup.
				randomState ^= randomState << 13
				randomState ^= randomState >> 7
				randomState ^= randomState << 17
				if replacement := randomState % samplesSeen; replacement < uint64(len(localLatencies)) {
					localLatencies[replacement] = latency
				}
			}
		}
		latencyMu.Lock()
		remaining := pf09MaxLatencySamples - len(latencies)
		if remaining > 0 {
			if len(localLatencies) > remaining {
				localLatencies = localLatencies[:remaining]
			}
			latencies = append(latencies, localLatencies...)
		}
		latencyMu.Unlock()
	})
	elapsed := time.Since(started)
	b.StopTimer()

	failureMu.Lock()
	err := firstFailure
	failureMu.Unlock()
	if err != nil {
		b.Fatalf("PF-09 send failed: %v", err)
	}
	successCount := successes.Load()
	if successCount == 0 || elapsed <= 0 || len(latencies) == 0 {
		b.Fatalf("PF-09 produced no measurable successful requests")
	}
	afterRPC := pf09RPCSnapshot(b)
	rpcDelta := afterRPC - beforeRPC
	if rpcDelta < 0 {
		b.Fatalf("PF-09 user_info_rpc_total moved backwards: before=%d after=%d", beforeRPC, afterRPC)
	}

	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	p95Index := int(math.Ceil(float64(len(latencies))*0.95)) - 1
	throughput := float64(successCount) / elapsed.Seconds()
	p95Micros := float64(latencies[p95Index]) / float64(time.Microsecond)
	rpcReduction := 100 * (1 - float64(rpcDelta)/float64(successCount))
	baselineRegression := 100 * (1 - throughput/baseline)

	b.ReportMetric(throughput, "msg/s")
	b.ReportMetric(p95Micros, "p95-us")
	b.ReportMetric(rpcReduction, "rpc-reduction-%")
	b.ReportMetric(baselineRegression, "baseline-regression-%")

	// Short calibration iterations are intentionally not gates. With the required
	// -benchtime=10s, the final iteration crosses this duration and enforces SLOs.
	if elapsed < gateMinDuration {
		return
	}
	if rpcReduction < pf09MinRPCReduction {
		b.Errorf("PF-09 %s RPC reduction %.2f%% is below %.2f%%", pf09StateName(state), rpcReduction, pf09MinRPCReduction)
	}
	if state == pf09L1 {
		if throughput < pf09MinThroughput {
			b.Errorf("PF-09 L1 throughput %.2f msg/s is below %.2f msg/s", throughput, pf09MinThroughput)
		}
		if baselineRegression > pf09MaxBaselineRegression {
			b.Errorf("PF-09 L1 throughput regressed %.2f%% from baseline %.2f msg/s", baselineRegression, baseline)
		}
	}
}

// pf09RPCSnapshot sums all configured Transmite instances. A comma-separated
// PF09_TRANSMITE_VARS_URLS is required for a multi-instance performance stack;
// the single-instance local stack falls back to tests/config.yaml.
func pf09RPCSnapshot(b *testing.B) int64 {
	b.Helper()
	raw := os.Getenv("PF09_TRANSMITE_VARS_URLS")
	if raw == "" {
		raw = HTTP.Config().Infra.TransmiteVars
	}
	var total int64
	var count int
	for _, endpoint := range strings.Split(raw, ",") {
		endpoint = strings.TrimSpace(endpoint)
		if endpoint == "" {
			continue
		}
		total += verify.BVar(b, strings.TrimRight(endpoint, "/"), "user_info_rpc_total")
		count++
	}
	if count == 0 {
		b.Fatal("PF-09 requires at least one Transmite bvar endpoint")
	}
	return total
}

func sendPF09Message(conversation pf09Conversation, sequence uint64) error {
	rsp := &transmite.SendMessageRsp{}
	err := conversation.sender.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: conversation.id,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{
				Text: fmt.Sprintf("pf09-%d", sequence),
			}},
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
			b.Fatalf("PF09_BASELINE_MSG_PER_SEC must be a finite value >= %.0f, got %q", pf09CheckedInBaseline, raw)
		}
		value = parsed
	}
	return value
}

func pf09GateMinDuration(b *testing.B) time.Duration {
	b.Helper()
	raw := os.Getenv("PF09_GATE_MIN_DURATION")
	if raw == "" {
		return pf09DefaultGateMinDuration
	}
	duration, err := time.ParseDuration(raw)
	if err != nil || duration <= 0 {
		b.Fatalf("PF09_GATE_MIN_DURATION must be a positive duration, got %q", raw)
	}
	return duration
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
