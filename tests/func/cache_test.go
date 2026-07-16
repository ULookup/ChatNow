//go:build func

package func_test

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	common "chatnow-tests/proto/chatnow/common"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

func userInfoBucket(uid string) uint32 {
	hash := uint32(2166136261)
	for i := 0; i < len(uid); i++ {
		hash ^= uint32(uid[i])
		hash *= 16777619
	}
	return hash % 64
}

func userInfoRedisKey(uid string) string {
	return fmt.Sprintf("im:user:{%d}:%s", userInfoBucket(uid), uid)
}

func redisRaw(t testing.TB, key string) []byte {
	t.Helper()
	container := HTTP.Config().Infra.RedisContainer
	if container == "" {
		container = "redis-node1"
	}
	out, err := exec.Command("docker", "exec", container, "redis-cli", "-c", "--raw", "GET", key).Output()
	require.NoError(t, err)
	return bytes.TrimSuffix(out, []byte("\n"))
}

func sendCacheTestMessageResult(user *client.HTTPClient, convID, suffix string) error {
	rsp := &transmite.SendMessageRsp{}
	err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "user-info-cache-" + suffix}},
		},
		ClientMsgId: client.NewRequestID(),
	}, rsp)
	if err != nil {
		return err
	}
	if !rsp.GetHeader().GetSuccess() {
		return fmt.Errorf("send failed: %s", rsp.GetHeader().GetErrorMessage())
	}
	return nil
}

func sendCacheTestMessage(t testing.TB, user *client.HTTPClient, convID, suffix string) {
	t.Helper()
	require.NoError(t, sendCacheTestMessageResult(user, convID, suffix))
}

type cacheTestWebSocket struct {
	conn net.Conn
}

func openCacheTestWebSocket(t testing.TB) *cacheTestWebSocket {
	t.Helper()
	addr := HTTP.Config().Target.WebsocketAddr
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	require.NoError(t, err)
	require.NoError(t, conn.SetDeadline(time.Now().Add(5*time.Second)))

	keyBytes := make([]byte, 16)
	_, err = rand.Read(keyBytes)
	require.NoError(t, err)
	key := base64.StdEncoding.EncodeToString(keyBytes)
	req := &http.Request{
		Method: "GET",
		URL:    &url.URL{Scheme: "http", Host: addr, Path: "/"},
		Host:   addr,
		Header: http.Header{
			"Connection":            {"Upgrade"},
			"Upgrade":               {"websocket"},
			"Sec-Websocket-Key":     {key},
			"Sec-Websocket-Version": {"13"},
		},
	}
	require.NoError(t, req.Write(conn))
	rsp, err := http.ReadResponse(bufio.NewReader(conn), req)
	require.NoError(t, err)
	require.Equal(t, http.StatusSwitchingProtocols, rsp.StatusCode)
	require.NoError(t, conn.SetDeadline(time.Time{}))
	return &cacheTestWebSocket{conn: conn}
}

func (ws *cacheTestWebSocket) close() {
	_ = ws.conn.Close()
}

func (ws *cacheTestWebSocket) writeBinary(t testing.TB, payload []byte) {
	t.Helper()
	frame := []byte{0x82}
	switch {
	case len(payload) < 126:
		frame = append(frame, 0x80|byte(len(payload)))
	case len(payload) <= 65535:
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	default:
		frame = append(frame, 0x80|127)
		var size [8]byte
		binary.BigEndian.PutUint64(size[:], uint64(len(payload)))
		frame = append(frame, size[:]...)
	}
	var mask [4]byte
	_, err := rand.Read(mask[:])
	require.NoError(t, err)
	frame = append(frame, mask[:]...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%len(mask)])
	}
	_, err = ws.conn.Write(frame)
	require.NoError(t, err)
}

func appendProtoString(dst []byte, field protowire.Number, value string) []byte {
	dst = protowire.AppendTag(dst, field, protowire.BytesType)
	return protowire.AppendString(dst, value)
}

func cacheTestAuthNotify(accessToken, deviceID string) []byte {
	var auth []byte
	auth = appendProtoString(auth, 1, accessToken)
	auth = appendProtoString(auth, 2, deviceID)
	var notify []byte
	notify = protowire.AppendTag(notify, 2, protowire.VarintType)
	notify = protowire.AppendVarint(notify, 49) // CLIENT_AUTH
	notify = protowire.AppendTag(notify, 10, protowire.BytesType)
	return protowire.AppendBytes(notify, auth)
}

func cacheTestHeartbeatNotify(uid string) []byte {
	var heartbeat []byte
	heartbeat = appendProtoString(heartbeat, 1, uid)
	var notify []byte
	notify = protowire.AppendTag(notify, 2, protowire.VarintType)
	notify = protowire.AppendVarint(notify, 51) // CLIENT_HEARTBEAT
	notify = protowire.AppendTag(notify, 9, protowire.BytesType)
	return protowire.AppendBytes(notify, heartbeat)
}

func requireJitteredRedisTTL(t testing.TB, key string, base time.Duration) time.Duration {
	t.Helper()
	ttl := verify.RedisTTL(t, key)
	require.GreaterOrEqual(t, ttl, base*8/10-2*time.Second, key)
	require.LessOrEqual(t, ttl, base*12/10+2*time.Second, key)
	return ttl
}

// FN-CA-04 | cache expirations are bounded, varied, and paired unacked keys
// share one randomized sample per push operation.
func TestFN_CA_TTLJitter(t *testing.T) {
	const sampleCount = 20
	const sessionTTL = 7 * 24 * time.Hour
	connections := make([]*cacheTestWebSocket, 0, sampleCount)
	t.Cleanup(func() {
		for _, ws := range connections {
			ws.close()
		}
	})

	t.Log("Session and Status DAOs have no production call sites; the ignored Task 5 DAO harness covers append/touch")

	t.Run("Codes", func(t *testing.T) {
		if os.Getenv("SMTP_HOST") == "" {
			t.Skip("SMTP_HOST is not configured; only the Codes reachable-path subtest is skipped")
		}
		codeRsp := &identity.SendVerifyCodeRsp{}
		require.NoError(t, HTTP.DoNoAuth("/service/identity/send_verify_code",
			&identity.SendVerifyCodeReq{
				RequestId: client.NewRequestID(),
				Destination: &identity.SendVerifyCodeReq_Email{
					Email: fmt.Sprintf("ttl-%s@example.com", strings.ToLower(client.NewRequestID())),
				},
			}, codeRsp))
		require.True(t, codeRsp.GetHeader().GetSuccess(), codeRsp.GetHeader().GetErrorMessage())
		requireJitteredRedisTTL(t, "im:code:"+codeRsp.GetVerifyCodeId(), 5*time.Minute)
	})

	t.Run("DeviceSet", func(t *testing.T) {
		deviceTTLs := make(map[time.Duration]struct{}, sampleCount)
		var heartbeatBefore time.Duration
		for i := 0; i < sampleCount; i++ {
			user, _, _ := fixture.RegisterAndLogin(t, HTTP)
			ws := openCacheTestWebSocket(t)
			connections = append(connections, ws)
			ws.writeBinary(t, cacheTestAuthNotify(user.AccessToken, "default_device"))

			deviceKey := fmt.Sprintf("im:dev:{%s}", user.UserID)
			require.Eventually(t, func() bool {
				return verify.RedisCLI(t, "EXISTS", deviceKey) == "1"
			}, 5*time.Second, 50*time.Millisecond, deviceKey)
			if i == 0 {
				verify.RedisCLI(t, "EXPIRE", deviceKey, "60")
				heartbeatBefore = verify.RedisTTL(t, deviceKey)
				ws.writeBinary(t, cacheTestHeartbeatNotify(user.UserID))
				require.Eventually(t, func() bool {
					seconds, err := strconv.ParseInt(verify.RedisCLI(t, "TTL", deviceKey), 10, 64)
					return err == nil && time.Duration(seconds)*time.Second > heartbeatBefore+24*time.Hour
				}, 5*time.Second, 50*time.Millisecond, "heartbeat must renew DeviceSet TTL")
			} else {
				ws.writeBinary(t, cacheTestHeartbeatNotify(user.UserID))
			}
			deviceTTLs[requireJitteredRedisTTL(t, deviceKey, sessionTTL)] = struct{}{}
		}
		require.GreaterOrEqual(t, len(deviceTTLs), 2,
			"20 independently randomized device TTLs must not all be identical")
	})

	t.Run("UnackedPush", func(t *testing.T) {
		sender, recipient, convID := fixture.MakeFriends(t, HTTP)
		recipientWS := openCacheTestWebSocket(t)
		connections = append(connections, recipientWS)
		recipientWS.writeBinary(t, cacheTestAuthNotify(recipient.AccessToken, "default_device"))
		deviceKey := fmt.Sprintf("im:dev:{%s}", recipient.UserID)
		require.Eventually(t, func() bool {
			return verify.RedisCLI(t, "EXISTS", deviceKey) == "1"
		}, 5*time.Second, 50*time.Millisecond, deviceKey)

		unackedKey := fmt.Sprintf("im:unack:{%s:default_device}", recipient.UserID)
		unackedIndexKey := fmt.Sprintf("im:unack:idx:{%s:default_device}", recipient.UserID)
		initial, err := strconv.Atoi(verify.RedisCLI(t, "ZCARD", unackedKey))
		require.NoError(t, err)
		unackedTTLs := make(map[time.Duration]struct{})
		for i := 0; i < 12; i++ {
			sendCacheTestMessage(t, sender, convID, fmt.Sprintf("ttl-jitter-%d", i))
			require.Eventually(t, func() bool {
				count, parseErr := strconv.Atoi(verify.RedisCLI(t, "ZCARD", unackedKey))
				return parseErr == nil && count >= initial+i+1
			}, 10*time.Second, 100*time.Millisecond, unackedKey)
			unackedTTL := requireJitteredRedisTTL(t, unackedKey, sessionTTL)
			indexTTL := requireJitteredRedisTTL(t, unackedIndexKey, sessionTTL)
			require.LessOrEqual(t, absDuration(unackedTTL-indexTTL), time.Second,
				"paired unacked keys must share one TTL sample")
			unackedTTLs[unackedTTL] = struct{}{}
		}
		require.GreaterOrEqual(t, len(unackedTTLs), 2,
			"repeated UnackedPush operations must produce varied paired TTL samples")
	})
}

func absDuration(value time.Duration) time.Duration {
	if value < 0 {
		return -value
	}
	return value
}

// FN-CA-05 | healthy Redis applies the distributed message rate limit.
func TestFN_CA_RateLimit(t *testing.T) {
	user, peer, convID := fixture.MakeFriends(t, HTTP)
	_ = peer

	rateLimited := 0
	for i := 0; i < 650; i++ {
		rsp := &transmite.SendMessageRsp{}
		err := user.DoAuth("/service/transmite/send", &transmite.SendMessageReq{
			RequestId:      client.NewRequestID(),
			ConversationId: convID,
			Content: &msg.MessageContent{
				Type: msg.MessageType_TEXT,
				Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("rate-limit-%d", i)}},
			},
			ClientMsgId: client.NewRequestID(),
		}, rsp)
		require.NoError(t, err)
		if rsp.GetHeader().GetErrorMessage() == "rate_limited" {
			rateLimited++
		}
	}

	require.Greater(t, rateLimited, 0, "healthy Redis must enforce the distributed rate limit")
}

func TestFN_CA_UserInfoL2AvoidsRepeatedRPC(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	verify.RedisCLI(t, "DEL", userInfoRedisKey(user.UserID))
	before := verify.BVar(t, HTTP.Config().Infra.TransmiteVars, "user_info_rpc_total")

	for i := 0; i < 20; i++ {
		sendCacheTestMessage(t, user, convID, fmt.Sprintf("repeat-%d", i))
	}

	after := verify.BVar(t, HTTP.Config().Infra.TransmiteVars, "user_info_rpc_total")
	require.LessOrEqual(t, after-before, int64(1))
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", userInfoRedisKey(user.UserID)))
}

func TestFN_CA_UserInfoSingleflight(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	verify.RedisCLI(t, "DEL", userInfoRedisKey(user.UserID))
	before := verify.BVar(t, HTTP.Config().Infra.TransmiteVars, "user_info_rpc_total")

	start := make(chan struct{})
	results := make(chan error, 200)
	var wg sync.WaitGroup
	for i := 0; i < 200; i++ {
		i := i
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			results <- sendCacheTestMessageResult(user, convID, fmt.Sprintf("flight-%d", i))
		}()
	}
	close(start)
	wg.Wait()
	close(results)
	for err := range results {
		require.NoError(t, err)
	}

	after := verify.BVar(t, HTTP.Config().Infra.TransmiteVars, "user_info_rpc_total")
	require.LessOrEqual(t, after-before, int64(1))
}

func TestFN_CA_UserInfoInvalidatedAfterProfileUpdate(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	key := userInfoRedisKey(user.UserID)
	verify.RedisCLI(t, "DEL", key)
	sendCacheTestMessage(t, user, convID, "warm")
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", key))

	newNickname := fmt.Sprintf("cache_%d", time.Now().UnixNano()%1_000_000_000)
	updateRsp := &identity.UpdateProfileRsp{}
	require.NoError(t, user.DoAuth("/service/identity/update_profile", &identity.UpdateProfileReq{
		RequestId: client.NewRequestID(),
		Nickname:  &newNickname,
	}, updateRsp))
	require.True(t, updateRsp.GetHeader().GetSuccess(), updateRsp.GetHeader().GetErrorMessage())
	require.Equal(t, "0", verify.RedisCLI(t, "EXISTS", key))

	// Identity can invalidate the shared L2 immediately; the process-local L1 is
	// intentionally bounded by its 45s TTL in the absence of a broadcast channel.
	time.Sleep(55 * time.Second)
	sendCacheTestMessage(t, user, convID, "after-update")
	serialized := redisRaw(t, key)
	info := &common.UserInfo{}
	require.NoError(t, proto.Unmarshal(serialized, info))
	require.Equal(t, newNickname, info.GetNickname())
}
