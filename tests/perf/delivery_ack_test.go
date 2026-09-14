//go:build perf

package perf_test

import (
	"os"
	"sort"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	push "chatnow-tests/proto/chatnow/push"
)

// PF-10 | P1 | Sample durable ACK latency and SQL work on an isolated stack.
func BenchmarkPF10_DeliveryACK(b *testing.B) {
	if os.Getenv("CHATNOW_ACK_BENCH") != "1" {
		b.Skip("requires an isolated stack and CHATNOW_ACK_BENCH=1")
	}
	if b.N > 200 {
		b.Fatal("use a bounded benchtime such as -benchtime=100x")
	}
	b.StopTimer()
	require.NoError(b, cleanup.WaitForStackReady(HTTP.Config(), 15*time.Second))
	b.Cleanup(func() { cleanup.CleanupAll(b, HTTP.Config()) })
	sender, recipient, convID := fixture.MakeFriends(b, HTTP)
	ws := fixture.ConnectWS(b, recipient)
	db := verify.NewDBVerifier(HTTP.Config().Database.MySQLDSN)
	b.Cleanup(db.Close)
	frames := make([]*push.NotifyMessage, b.N)
	acks := make([]*push.NotifyMsgPushAck, b.N)
	for i := range frames {
		fixture.SendTextMessage(b, sender, convID, "ack-benchmark")
		acks[i] = fixture.WaitDeliveryACK(b, recipient, ws)
		frames[i] = &push.NotifyMessage{
			NotifyType:    push.NotifyType_MSG_PUSH_ACK,
			NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: acks[i]},
		}
	}
	questions := db.QuestionCount(b)
	polls := uint64(0)
	latencies := make([]time.Duration, b.N)
	started := time.Now()
	b.StartTimer()
	for i, frame := range frames {
		start := time.Now()
		require.NoError(b, ws.SendNotify(frame))
		for {
			polls++
			if db.ReadLastAckSeq(b, recipient.UserID, convID) == acks[i].SeqId {
				break
			}
			if time.Since(start) > 5*time.Second {
				b.Fatal("delivery ACK did not converge")
			}
			time.Sleep(time.Millisecond)
		}
		latencies[i] = time.Since(start)
	}
	b.StopTimer()
	elapsed := time.Since(started)
	statementDelta := db.QuestionCount(b) - questions
	serverStatements := uint64(0)
	if statementDelta > polls+1 {
		serverStatements = statementDelta - polls - 1
	}
	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	b.ReportMetric(float64(b.N)/elapsed.Seconds(), "ack/s")
	b.ReportMetric(float64(serverStatements)/elapsed.Seconds(), "server-SQL/s")
	b.ReportMetric(float64(serverStatements)/float64(b.N), "server-SQL/ack")
	b.ReportMetric(float64(latencies[(b.N-1)*95/100].Microseconds()), "p95-us")
	b.ReportMetric(float64(latencies[(b.N-1)*99/100].Microseconds()), "p99-us")
	key := "im:unack:idx:{" + recipient.UserID + ":" + recipient.DeviceID + "}"
	for _, ack := range acks {
		require.Eventually(b, func() bool {
			return verify.RedisCLI(b, "HEXISTS", key, strconv.FormatUint(ack.UserSeq, 10)) == "0"
		}, 5*time.Second, 20*time.Millisecond)
	}
}
