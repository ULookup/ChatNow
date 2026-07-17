//go:build reliability

package reliability_test

import (
	"bytes"
	"fmt"
	"net"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/chaos"
	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
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
	openedBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_open_total")
	rejectedBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total")
	recoveredBefore := reliabilitySumBVar(t, endpoints, "redis_circuit_recovered_total")

	t.Cleanup(func() {
		chaos.StartRedisCluster(t, HTTP.Config())
		chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
	})
	chaos.StopRedisCluster(t, HTTP.Config())

	rateLimited := 0
	seqUnavailable := 0
	for i := 0; i < 650; i++ {
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
		if sendRsp.GetHeader().GetErrorMessage() == "rate_limited" {
			rateLimited++
		}
		if sendRsp.GetHeader().GetErrorMessage() == "序号生成失败" {
			seqUnavailable++
		}
	}
	require.Greater(t, rateLimited, 0, "Redis outage must retain bounded rate limiting")
	require.Greater(t, seqUnavailable, 0, "SeqGen truth source must fail unavailable")
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_open_total"), openedBefore)
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_rejected_total"), rejectedBefore)

	uid := user.UserID
	for i := 0; i < 3; i++ {
		rsp := &identity.GetProfileRsp{}
		_ = user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
			RequestId: client.NewRequestID(), UserId: &uid,
		}, rsp)
	}

	started := time.Now()
	fastFail := &transmite.SendMessageRsp{}
	err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "fast-fail"}}},
		ClientMsgId: client.NewRequestID(),
	}, fastFail)
	require.NoError(t, err)
	require.False(t, fastFail.GetHeader().GetSuccess())
	require.Less(t, time.Since(started), 50*time.Millisecond)

	chaos.StartRedisCluster(t, HTTP.Config())
	chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
	time.Sleep(1100 * time.Millisecond)
	rsp := &identity.GetProfileRsp{}
	require.NoError(t, user.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{
		RequestId: client.NewRequestID(), UserId: &uid,
	}, rsp))
	require.True(t, rsp.GetHeader().GetSuccess())

	recoveredSend := &transmite.SendMessageRsp{}
	require.NoError(t, user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "recovered"}}},
		ClientMsgId: client.NewRequestID(),
	}, recoveredSend))
	require.True(t, recoveredSend.GetHeader().GetSuccess(), recoveredSend.GetHeader().GetErrorMessage())
	require.Greater(t, reliabilitySumBVar(t, endpoints, "redis_circuit_recovered_total"), recoveredBefore,
		"an Open->HalfOpen probe must recover the Transmite Redis circuit")
}

// RL-05 Push truth source: Rabbit accepts while Push is paused; after Redis is
// stopped, resuming Push must requeue before websocket delivery. Recovery then
// permits the half-open probe, durable Unacked write, and at-least-once delivery.
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
	require.NoError(t, ws.WriteBinary(client.PushAuthNotify(recipient.AccessToken, "default_device")))

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

	// A successful end-to-end delivery warms that instance's route L1 before it
	// is paused. The test-only TTL keeps the route through the outage choreography.
	warmMarker := "rl-unacked-warm-" + client.NewRequestID()
	sendReliabilityMessage(t, sender, convID, warmMarker)
	require.True(t, readWebSocketMarker(ws, warmMarker, 5*time.Second), "warm Push delivery missing")
	persistBefore := verify.BVar(t, pushEndpoint, "push_unacked_persist_failure_total")
	requeueBefore := verify.BVar(t, pushEndpoint, "push_message_requeue_total")

	requireDocker(t, "pause", pushContainer)
	paused := true
	redisStopped := false
	t.Cleanup(func() {
		if paused {
			_, _ = exec.Command("docker", "unpause", pushContainer).CombinedOutput()
		}
		if redisStopped {
			chaos.StartRedisCluster(t, HTTP.Config())
			chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
		}
	})

	marker := "rl-unacked-" + client.NewRequestID()
	sendReliabilityMessage(t, sender, convID, marker)

	chaos.StopRedisCluster(t, HTTP.Config())
	redisStopped = true
	requireDocker(t, "unpause", pushContainer)
	paused = false
	requireBVarIncrease(t, pushEndpoint, "push_unacked_persist_failure_total", persistBefore, 5*time.Second)
	requireBVarIncrease(t, pushEndpoint, "push_message_requeue_total", requeueBefore, 5*time.Second)
	if payload, readErr := ws.ReadFrame(500 * time.Millisecond); readErr == nil {
		t.Fatalf("Push delivered before durable Unacked persistence: %x", payload)
	} else if timeout, ok := readErr.(net.Error); !ok || !timeout.Timeout() {
		t.Fatalf("websocket failed while awaiting Redis outage: %v", readErr)
	}

	chaos.StartRedisCluster(t, HTTP.Config())
	chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
	redisStopped = false

	require.True(t, readWebSocketMarker(ws, marker, 20*time.Second),
		"requeued Push was not delivered after Redis recovery")
	unackedKey := fmt.Sprintf("im:unack:{%s:default_device}", recipient.UserID)
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
