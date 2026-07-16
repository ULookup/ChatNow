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

	pushContainer := HTTP.Config().Infra.PushContainer
	require.NotEmpty(t, pushContainer)
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
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, sender.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId: client.NewRequestID(), ConversationId: convID,
		Content: &msg.MessageContent{Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: marker}}},
		ClientMsgId: client.NewRequestID(),
	}, sendRsp))
	require.True(t, sendRsp.GetHeader().GetSuccess(), sendRsp.GetHeader().GetErrorMessage())

	chaos.StopRedisCluster(t, HTTP.Config())
	redisStopped = true
	requireDocker(t, "unpause", pushContainer)
	paused = false
	if payload, readErr := ws.ReadFrame(2 * time.Second); readErr == nil {
		t.Fatalf("Push delivered before durable Unacked persistence: %x", payload)
	} else if timeout, ok := readErr.(net.Error); !ok || !timeout.Timeout() {
		t.Fatalf("websocket failed while awaiting Redis outage: %v", readErr)
	}

	chaos.StartRedisCluster(t, HTTP.Config())
	chaos.WaitRedisCluster(t, HTTP.Config(), 60*time.Second)
	redisStopped = false

	delivered := false
	deadline = time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		payload, readErr := ws.ReadFrame(time.Until(deadline))
		if readErr != nil {
			break
		}
		if bytes.Contains(payload, []byte(marker)) {
			delivered = true
			break
		}
	}
	require.True(t, delivered, "requeued Push was not delivered after Redis recovery")
	unackedKey := fmt.Sprintf("im:unack:{%s:default_device}", recipient.UserID)
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", unackedKey),
		"delivery must follow durable Unacked persistence")
}

func requireDocker(t testing.TB, args ...string) {
	t.Helper()
	if out, err := exec.Command("docker", args...).CombinedOutput(); err != nil {
		t.Fatalf("docker %v: %v: %s", args, err, out)
	}
}

func reliabilityTransmiteEndpoints(t testing.TB) []string {
	t.Helper()
	var endpoints []string
	for _, raw := range strings.Split(HTTP.Config().Infra.TransmiteVars, ",") {
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
