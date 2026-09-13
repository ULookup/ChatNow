//go:build reliability

package reliability_test

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	authmeta "chatnow-tests/proto/chatnow/common/auth"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// RL-05 | P0 | Redis 熔断后快速失败并自动恢复
func TestRL_RedisCircuitFastFailAndRecovery(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	peer, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, user, []*client.HTTPClient{peer}, "rl-redis-outage")
	prewarmRsp := &transmite.SendMessageRsp{}
	require.NoError(t, user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "prewarm"}},
		},
		ClientMsgId: client.NewRequestID(),
	}, prewarmRsp))
	require.True(t, prewarmRsp.GetHeader().GetSuccess())
	endpoints := reliabilityTransmiteEndpoints(t)
	require.Len(t, endpoints, 1, "RL-05 requires one isolated Transmite for per-request circuit evidence")
	metadata, err := proto.Marshal(&authmeta.RpcMetadata{
		TraceId: client.NewRequestID(), UserId: user.UserID, DeviceId: user.DeviceID,
	})
	require.NoError(t, err)
	internalSend := func(marker string) (*transmite.SendMessageRsp, error) {
		rsp := &transmite.SendMessageRsp{}
		err := user.DoInternalRPC(endpoints[0], "chatnow.transmite.MsgTransmitService", "SendMessage",
			&transmite.SendMessageReq{
				RequestId: client.NewRequestID(), ConversationId: convID, ClientMsgId: client.NewRequestID(),
				Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: marker}}},
			}, rsp, metadata)
		return rsp, err
	}
	internalWarm, err := internalSend("internal-prewarm")
	require.NoError(t, err)
	require.True(t, internalWarm.GetHeader().GetSuccess(), "direct RPC control must reach the real service")
	openedBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_open_total")
	rejectedBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total")
	recoveredBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_recovered_total")

	redisPaused := true
	// The same idempotency key bounds recovery writes even after an ambiguous
	// transport response. Redis cluster health alone does not close RPC circuits.
	recoveryID := client.NewRequestID()
	restore := func() {
		if redisPaused {
			chaos.UnpauseRedisCluster(t, HTTP.Config())
			chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
			redisPaused = false
		}
		uid := user.UserID
		require.Eventually(t, func() bool {
			profile := &identity.GetProfileRsp{}
			if user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
				RequestId: client.NewRequestID(), UserId: &uid,
			}, profile) != nil || !profile.GetHeader().GetSuccess() {
				return false
			}
			sent := &transmite.SendMessageRsp{}
			return user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
				RequestId: client.NewRequestID(), ConversationId: convID, ClientMsgId: recoveryID,
				Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "recovered"}}},
			}, sent) == nil && sent.GetHeader().GetSuccess()
		}, 30*time.Second, 100*time.Millisecond, "Redis cleanup must restore account and message RPCs")
	}
	t.Cleanup(restore)
	chaos.PauseRedisCluster(t, HTTP.Config())

	rateLimited := 0
	seqUnavailable := 0
	responseCodes := make(map[int32]int)
	outageStarted := time.Now()
	// The dedicated RL-05 stack uses 8 user tokens per minute. A bounded
	// burst exhausts fallback capacity even with refill during failed Redis calls.
	for i := 0; i < 32; i++ {
		sendRsp := &transmite.SendMessageRsp{}
		err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
			RequestId:      client.NewRequestID(),
			ConversationId: convID,
			Content: &msg.MessageContent{
				Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("outage-%d", i)}},
			},
			ClientMsgId: client.NewRequestID(),
		}, sendRsp)
		require.NoError(t, err)
		responseCodes[sendRsp.GetHeader().GetErrorCode()]++
		if sendRsp.GetHeader().GetErrorMessage() == "rate_limited" {
			rateLimited++
		}
		if sendRsp.GetHeader().GetErrorMessage() == "序号生成失败" {
			seqUnavailable++
		}
	}
	t.Logf("outage responses: codes=%v rate_limited=%d seq_unavailable=%d elapsed=%s",
		responseCodes, rateLimited, seqUnavailable, time.Since(outageStarted))
	require.Greater(t, rateLimited, 0, "Redis outage must retain bounded rate limiting")
	require.Greater(t, seqUnavailable, 0, "SeqGen truth source must fail unavailable")
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_open_total"), openedBefore)
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total"), rejectedBefore)

	// A Gateway call includes a separate Redis circuit. Measure the Transmite
	// boundary directly and classify every sample using its actual counters.
	// No slow Open rejection can be discarded by retrying for a faster sample.
	// This delay deliberately admits a recovery probe; it is not readiness
	// evidence. Counter deltas below must prove that the probe actually ran.
	time.Sleep(1100 * time.Millisecond)
	openSamples, probeSamples := 0, 0
	for i := 0; i < 8 && openSamples < 3; i++ {
		failures := reliabilitySumBVar(t, endpoints, "redis_call_failure_total")
		rejections := reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total")
		opens := reliabilitySumBVar(t, endpoints, "redis_circuit_open_total")
		started := time.Now()
		failed, err := internalSend("fast-fail")
		elapsed := time.Since(started)
		require.NoError(t, err)
		require.False(t, failed.GetHeader().GetSuccess())
		failureDelta := reliabilitySumBVar(t, endpoints, "redis_call_failure_total") - failures
		rejectedDelta := reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total") - rejections
		openDelta := reliabilitySumBVar(t, endpoints, "redis_circuit_open_total") - opens
		t.Logf("Transmite circuit sample %d: elapsed=%s failures=%d rejected=%d opened=%d",
			i, elapsed, failureDelta, rejectedDelta, openDelta)
		if failureDelta > 0 {
			require.Positive(t, openDelta, "an admitted recovery attempt must reopen the paused Redis circuit")
			probeSamples++
			continue
		}
		require.Zero(t, failureDelta, "counters must be monotonic")
		require.Zero(t, openDelta, "Open rejection must not change circuit generation")
		require.Positive(t, rejectedDelta, "sample must prove an actual circuit rejection")
		require.Less(t, elapsed, 50*time.Millisecond, "every proven Open rejection must meet the latency bound")
		openSamples++
	}
	require.Equal(t, 3, openSamples, "insufficient phase-proven Open rejections in bounded sample set")
	require.Positive(t, probeSamples, "the expired Open interval must exercise an actual recovery probe")
	if os.Getenv("CHATNOW_REDIS_CLEANUP_CHILD") == "1" {
		t.Fatal("injected RL-05 assertion failure")
	}

	uid := user.UserID
	for i := 0; i < 3; i++ {
		rsp := &identity.GetProfileRsp{}
		_ = user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
			RequestId: client.NewRequestID(), UserId: &uid,
		}, rsp)
	}

	restore()
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_recovered_total"), recoveredBefore,
		"an Open->HalfOpen probe must recover the Transmite Redis circuit")
}

// RL-REDIS-01 | P0 | Assertion failure must restore actual message availability.
func TestRL_RedisCircuitCleanupAfterAssertionFailure(t *testing.T) {
	sender, _, _ := fixture.RegisterAndLogin(t, HTTP)
	peer, _, _ := fixture.RegisterAndLogin(t, HTTP)
	convID := fixture.CreateGroupWithMembers(t, sender, []*client.HTTPClient{peer}, "rl-cleanup")
	sendReliabilityMessage(t, sender, convID, "cleanup-prewarm")
	// The parent owns emergency unpause if the bounded child is interrupted.
	childCompleted := false
	t.Cleanup(func() {
		if !childCompleted {
			chaos.UnpauseRedisCluster(t, HTTP.Config())
			chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
		}
	})
	executable, err := os.Executable()
	require.NoError(t, err)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, executable, "-test.run=^TestRL_RedisCircuitFastFailAndRecovery$", "-test.v", "-test.count=1")
	cmd.Env = append(os.Environ(), "CHATNOW_REDIS_CLEANUP_CHILD=1")
	out, err := cmd.CombinedOutput()
	if exit, ok := err.(*exec.ExitError); ok && exit.ExitCode() == 1 {
		childCompleted = true // Go completed all test cleanups before exit(1).
	}
	require.Error(t, err, "child must execute the injected failure")
	require.Contains(t, string(out), "injected RL-05 assertion failure", "child failed before the cleanup boundary")
	// Do not poll here: the child's cleanup owns the convergence deadline.
	sendReliabilityMessage(t, sender, convID, "cleanup-confirmed")
}

// RL-05 Push truth source: Rabbit accepts while Push is paused; after Redis is
// paused without withdrawing DNS, resuming Push must requeue before delivery.
// Recovery permits the half-open probe, durable Unacked write, and delivery.
func TestRL_PushUnackedRequeuesUntilRedisRecovers(t *testing.T) {
	// This black-box choreography is intentionally tied to the compose topology:
	// one Push container owns the websocket and exposes the sole configured bvar
	// endpoint. Multi-instance CI must provide a dedicated single-Push test stack.
	pushEndpoint := reliabilitySinglePushEndpoint(t)
	pushContainer := HTTP.Config().Infra.PushContainer
	require.NotEmpty(t, pushContainer,
		"pause-container Push test requires the websocket-owning container name")
	require.GreaterOrEqual(t, HTTP.Config().Infra.PushRouteL1TTLSec, 15,
		"Push reliability stack requires route L1 TTL >=15s; docker config uses 30s")

	sender, recipient, convID := fixture.MakeFriends(t, HTTP)
	ws, err := client.OpenWebSocket(HTTP.Config())
	require.NoError(t, err)
	t.Cleanup(func() { _ = ws.Close() })
	require.NoError(t, ws.WriteBinary(client.PushAuthNotify(recipient.AccessToken, recipient.DeviceID)))
	senderWS, err := client.OpenWebSocket(HTTP.Config())
	require.NoError(t, err)
	t.Cleanup(func() { _ = senderWS.Close() })
	require.NoError(t, senderWS.WriteBinary(client.PushAuthNotify(sender.AccessToken, sender.DeviceID)))

	deviceKey := fmt.Sprintf("im:dev:{%s}", recipient.UserID)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) && verify.RedisCLI(t, "EXISTS", deviceKey) != "1" {
		time.Sleep(50 * time.Millisecond)
	}
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", deviceKey))
	drainDeadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(drainDeadline) {
		_, readErr := ws.ReadFrame(100 * time.Millisecond)
		if timeout, ok := readErr.(net.Error); ok && timeout.Timeout() {
			break
		}
		require.NoError(t, readErr)
	}

	// Warm both member routes: an offline sender has no positive L1 route and
	// would fail route discovery before the Unacked persistence under test.
	// The test-only TTL keeps both routes through the outage choreography.
	warmMarker := "rl-unacked-warm-" + client.NewRequestID()
	sendReliabilityMessage(t, sender, convID, warmMarker)
	require.True(t, readWebSocketMarker(ws, warmMarker, 5*time.Second), "warm Push delivery missing")
	require.True(t, readWebSocketMarker(senderWS, warmMarker, 5*time.Second), "warm sender Push delivery missing")
	persistBefore := verify.BVar(t, pushEndpoint, "push_unacked_persist_failure_total")
	requeueBefore := verify.BVar(t, pushEndpoint, "push_message_requeue_total")

	requireDocker(t, "pause", pushContainer)
	paused := true
	redisPaused := false
	t.Cleanup(func() {
		if paused {
			_, _ = exec.Command("docker", "unpause", pushContainer).CombinedOutput()
		}
		if redisPaused {
			chaos.UnpauseRedisCluster(t, HTTP.Config())
			chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
		}
	})

	marker := "rl-unacked-" + client.NewRequestID()
	sendReliabilityMessage(t, sender, convID, marker)

	redisPaused = true
	chaos.PauseRedisCluster(t, HTTP.Config())
	requireDocker(t, "unpause", pushContainer)
	paused = false
	requireBVarIncrease(t, pushEndpoint, "push_unacked_persist_failure_total", persistBefore, 5*time.Second)
	requireBVarIncrease(t, pushEndpoint, "push_message_requeue_total", requeueBefore, 5*time.Second)
	// Other already durable messages may be redelivered; only the newly queued
	// marker is forbidden before its own Unacked write succeeds.
	require.False(t, readWebSocketMarker(ws, marker, 500*time.Millisecond),
		"Push delivered the outage message before durable Unacked persistence")

	chaos.UnpauseRedisCluster(t, HTTP.Config())
	chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
	redisPaused = false

	require.True(t, readWebSocketMarker(ws, marker, 20*time.Second),
		"requeued Push was not delivered after Redis recovery")
	unackedKey := fmt.Sprintf("im:unack:{%s:%s}", recipient.UserID, recipient.DeviceID)
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", unackedKey),
		"delivery must follow durable Unacked persistence")
}

func sendReliabilityMessage(t testing.TB, sender *client.HTTPClient, convID, marker string) {
	t.Helper()
	rsp := &transmite.SendMessageRsp{}
	require.NoError(t, sender.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: marker}}},
		ClientMsgId: client.NewRequestID(),
	}, rsp))
	require.True(t, rsp.GetHeader().GetSuccess(), rsp.GetHeader().GetErrorMessage())
}

func readWebSocketMarker(ws *client.WebSocket, marker string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		payload, err := ws.ReadFrame(time.Until(deadline))
		if err != nil {
			return false
		}
		if bytes.Contains(payload, []byte(marker)) {
			return true
		}
	}
	return false
}

func requireBVarIncrease(t testing.TB, endpoint, metric string, before int64, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if verify.BVar(t, endpoint, metric) > before {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("%s did not increase from %d", metric, before)
}

func requireDocker(t testing.TB, args ...string) {
	t.Helper()
	if out, err := exec.Command("docker", args...).CombinedOutput(); err != nil {
		t.Fatalf("docker %v: %v: %s", args, err, out)
	}
}

func reliabilityTransmiteEndpoints(t testing.TB) []string {
	t.Helper()
	return reliabilityEndpoints(t, HTTP.Config().Infra.TransmiteVars)
}

func reliabilitySinglePushEndpoint(t testing.TB) string {
	t.Helper()
	endpoints := reliabilityEndpoints(t, HTTP.Config().Infra.PushVars)
	require.Len(t, endpoints, 1,
		"pause-container Push test requires one bvar endpoint for the websocket-owning container; use a dedicated single-instance Push test stack")
	return endpoints[0]
}

func reliabilityEndpoints(t testing.TB, rawEndpoints string) []string {
	t.Helper()
	var endpoints []string
	for _, raw := range strings.Split(rawEndpoints, ",") {
		if endpoint := strings.TrimRight(strings.TrimSpace(raw), "/"); endpoint != "" {
			endpoints = append(endpoints, endpoint)
		}
	}
	require.NotEmpty(t, endpoints)
	return endpoints
}

func reliabilitySumBVar(t testing.TB, endpoints []string, metric string) int64 {
	t.Helper()
	var total int64
	for _, endpoint := range endpoints {
		total += verify.BVar(t, endpoint, metric)
	}
	return total
}
