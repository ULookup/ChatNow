//go:build func

package func_test

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	common "chatnow-tests/proto/chatnow/common"
	identity "chatnow-tests/proto/chatnow/identity"
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
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

type cacheTestWebSocket = client.WebSocket

func openCacheTestWebSocket(t testing.TB) *cacheTestWebSocket {
	t.Helper()
	ws, err := client.OpenWebSocket(HTTP.Config())
	require.NoError(t, err)
	return ws
}

func writeCacheTestBinary(t testing.TB, ws *cacheTestWebSocket, payload []byte) {
	t.Helper()
	require.NoError(t, ws.WriteBinary(payload))
}

func appendProtoString(dst []byte, field protowire.Number, value string) []byte {
	dst = protowire.AppendTag(dst, field, protowire.BytesType)
	return protowire.AppendString(dst, value)
}

func cacheTestAuthNotify(accessToken, deviceID string) []byte {
	return client.PushAuthNotify(accessToken, deviceID)
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

var _ func(...string) (string, error) = verify.RedisCLIResult

func readRepoSource(t testing.TB, relativePath string) string {
	t.Helper()
	_, currentFile, _, ok := runtime.Caller(0)
	require.True(t, ok, "locate cache_test.go")
	repoRoot := filepath.Clean(filepath.Join(filepath.Dir(currentFile), "..", ".."))
	content, err := os.ReadFile(filepath.Join(repoRoot, filepath.FromSlash(relativePath)))
	require.NoError(t, err, relativePath)
	return string(content)
}

func normalizedSourceSection(source, start, end string) (string, error) {
	startAt := strings.Index(source, start)
	if startAt < 0 {
		return "", fmt.Errorf("missing source section %q", start)
	}
	endAt := strings.Index(source[startAt+len(start):], end)
	if endAt < 0 {
		return "", fmt.Errorf("missing end marker %q for %q", end, start)
	}
	section := source[startAt : startAt+len(start)+endAt]
	return strings.Join(strings.Fields(section), " "), nil
}

func requireSourceFragments(sectionName, section string, fragments ...string) error {
	for _, fragment := range fragments {
		normalized := strings.Join(strings.Fields(fragment), " ")
		if !strings.Contains(section, normalized) {
			return fmt.Errorf("%s missing contract fragment %q", sectionName, normalized)
		}
	}
	return nil
}

func checkCacheTTLSourceContracts(source string) error {
	sections := make(map[string]string)
	markers := [][3]string{
		{"Session", "class Session", "class Status"},
		{"Status", "class Status", "class Codes"},
	}
	for _, marker := range markers {
		section, err := normalizedSourceSection(source, marker[1], marker[2])
		if err != nil {
			return err
		}
		sections[marker[0]] = section
	}

	rules := []struct {
		name      string
		fragments []string
	}{
		{"Session", []string{
			"_c->set(key::kSession + ssid, uid, randomized_ttl(ttl))",
			"_c->expire(key::kSession + ssid, randomized_ttl(ttl))",
		}},
		{"Status", []string{
			"_c->set(key::kStatus + uid, \"1\", randomized_ttl(ttl))",
			"_c->expire(key::kStatus + uid, randomized_ttl(ttl))",
		}},
	}
	for _, rule := range rules {
		if err := requireSourceFragments(rule.name, sections[rule.name], rule.fragments...); err != nil {
			return err
		}
	}

	return nil
}

func requireCacheTTLSourceContracts(t testing.TB, source string) {
	t.Helper()
	require.NoError(t, checkCacheTTLSourceContracts(source))
}

func TestFN_CA_LegacyUnusedTTLSourceContract(t *testing.T) {
	daoSource := readRepoSource(t, "common/dao/data_redis.hpp")
	requireCacheTTLSourceContracts(t, daoSource)

	t.Run("rejects fixed Session TTL", func(t *testing.T) {
		mutant := strings.Replace(daoSource, "randomized_ttl(ttl)", "ttl", 1)
		require.Error(t, checkCacheTTLSourceContracts(mutant))
	})
}

type redisResultPredicate func(string) (bool, error)

func requireEventuallyRedis(t testing.TB, timeout, interval time.Duration, message string,
	predicate redisResultPredicate, args ...string) string {
	t.Helper()
	var mu sync.Mutex
	var lastResult string
	var lastErr error
	matched := assert.Eventually(t, func() bool {
		result, err := verify.RedisCLIResult(args...)
		if err == nil {
			var predicateMatch bool
			predicateMatch, err = predicate(result)
			mu.Lock()
			lastResult, lastErr = result, err
			mu.Unlock()
			return predicateMatch && err == nil
		}
		mu.Lock()
		lastResult, lastErr = result, err
		mu.Unlock()
		return false
	}, timeout, interval, message)

	mu.Lock()
	result, err := lastResult, lastErr
	mu.Unlock()
	require.NoError(t, err, "%s (last result %q)", message, result)
	require.True(t, matched, "%s (last result %q)", message, result)
	return result
}

func redisEquals(expected string) redisResultPredicate {
	return func(result string) (bool, error) { return result == expected, nil }
}

// FN-CA-04 | cache expirations are bounded, varied, and paired unacked keys
// share one randomized sample per push operation.
func TestFN_CA_TTLJitter(t *testing.T) {
	const sampleCount = 20
	const sessionTTL = 7 * 24 * time.Hour
	connections := make([]*cacheTestWebSocket, 0, sampleCount)
	t.Cleanup(func() {
		for _, ws := range connections {
			_ = ws.Close()
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
			writeCacheTestBinary(t, ws, cacheTestAuthNotify(user.AccessToken, "default_device"))

			deviceKey := fmt.Sprintf("im:dev:{%s}", user.UserID)
			requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
				"DeviceSet key must exist", redisEquals("1"), "EXISTS", deviceKey)
			if i == 0 {
				verify.RedisCLI(t, "EXPIRE", deviceKey, "60")
				heartbeatBefore = verify.RedisTTL(t, deviceKey)
				writeCacheTestBinary(t, ws, cacheTestHeartbeatNotify(user.UserID))
				requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
					"heartbeat must renew DeviceSet TTL", func(result string) (bool, error) {
						seconds, err := strconv.ParseInt(result, 10, 64)
						return time.Duration(seconds)*time.Second > heartbeatBefore+24*time.Hour, err
					}, "TTL", deviceKey)
			} else {
				writeCacheTestBinary(t, ws, cacheTestHeartbeatNotify(user.UserID))
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
		writeCacheTestBinary(t, recipientWS, cacheTestAuthNotify(recipient.AccessToken, "default_device"))
		deviceKey := fmt.Sprintf("im:dev:{%s}", recipient.UserID)
		requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
			"recipient DeviceSet key must exist", redisEquals("1"), "EXISTS", deviceKey)

		unackedKey := fmt.Sprintf("im:unack:{%s:default_device}", recipient.UserID)
		unackedIndexKey := fmt.Sprintf("im:unack:idx:{%s:default_device}", recipient.UserID)
		initial, err := strconv.Atoi(verify.RedisCLI(t, "ZCARD", unackedKey))
		require.NoError(t, err)
		unackedTTLs := make(map[time.Duration]struct{})
		for i := 0; i < 12; i++ {
			sendCacheTestMessage(t, sender, convID, fmt.Sprintf("ttl-jitter-%d", i))
			requireEventuallyRedis(t, 10*time.Second, 100*time.Millisecond,
				"UnackedPush ZSET must receive the message", func(result string) (bool, error) {
					count, err := strconv.Atoi(result)
					return count >= initial+i+1, err
				}, "ZCARD", unackedKey)
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

func TestFN_CA_UnackedSameUserSeqLatestPayloadAndAck(t *testing.T) {
	recipient, _, _ := fixture.RegisterAndLogin(t, HTTP)
	ws := openCacheTestWebSocket(t)
	t.Cleanup(func() { _ = ws.Close() })
	const deviceID = "default_device"
	writeCacheTestBinary(t, ws, cacheTestAuthNotify(recipient.AccessToken, deviceID))

	deviceKey := fmt.Sprintf("im:dev:{%s}", recipient.UserID)
	requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
		"Push route must exist before direct PushToUser calls", redisEquals("1"),
		"EXISTS", deviceKey)

	userSeq := uint64(time.Now().UnixNano())
	unackedKey := fmt.Sprintf("im:unack:{%s:%s}", recipient.UserID, deviceID)
	unackedIndexKey := fmt.Sprintf("im:unack:idx:{%s:%s}", recipient.UserID, deviceID)
	verify.RedisCLI(t, "DEL", unackedKey, unackedIndexKey)

	first := &push.NotifyMessage{
		NotifyEventId: proto.String("unacked-first-" + client.NewRequestID()),
		NotifyType:    push.NotifyType_TYPING_NOTIFY,
		NotifyRemarks: &push.NotifyMessage_Typing{Typing: &push.NotifyTyping{
			UserId: recipient.UserID, ConversationId: "unacked-contract", IsTyping: false,
		}},
	}
	second := proto.Clone(first).(*push.NotifyMessage)
	second.NotifyEventId = proto.String("unacked-second-" + client.NewRequestID())
	second.GetTyping().IsTyping = true

	for _, notify := range []*push.NotifyMessage{first, second} {
		rsp := &push.PushToUserRsp{}
		require.NoError(t, HTTP.DoProtobufURL(
			HTTP.Config().Infra.PushVars+"/chatnow.push.PushService/PushToUser",
			&push.PushToUserReq{
				RequestId: client.NewRequestID(), UserId: recipient.UserID,
				Notify: notify, UserSeq: proto.Uint64(userSeq), TargetDeviceIds: []string{deviceID},
			}, rsp))
		require.True(t, rsp.GetHeader().GetSuccess(), rsp.GetHeader().GetErrorMessage())
		require.Equal(t, int32(1), rsp.GetOnlineDeviceCount())
	}

	seq := strconv.FormatUint(userSeq, 10)
	requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
		"same user_seq must retain one stable ZSET identity", redisEquals("1"),
		"ZCARD", unackedKey)
	require.Equal(t, seq, verify.RedisCLI(t, "ZRANGE", unackedKey, "0", "-1"))
	require.Equal(t, "1", verify.RedisCLI(t, "HLEN", unackedIndexKey))

	encodedLatest := verify.RedisCLI(t, "HGET", unackedIndexKey, seq)
	latestBytes, err := base64.StdEncoding.DecodeString(encodedLatest)
	require.NoError(t, err)
	latest := &push.NotifyMessage{}
	require.NoError(t, proto.Unmarshal(latestBytes, latest))
	require.True(t, proto.Equal(second, latest),
		"same user_seq must replace the HASH payload with the second notification")

	ackBytes, err := proto.Marshal(&push.NotifyMessage{
		NotifyType: push.NotifyType_MSG_PUSH_ACK,
		NotifyRemarks: &push.NotifyMessage_MsgPushAck{MsgPushAck: &push.NotifyMsgPushAck{
			UserId: recipient.UserID, DeviceId: deviceID, MessageId: 1,
			UserSeq: userSeq, ConversationId: "unacked-contract", SeqId: 0, // typing push has no conversation watermark
		}},
	})
	require.NoError(t, err)
	writeCacheTestBinary(t, ws, ackBytes)
	requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
		"WebSocket ACK must remove the ZSET identity", redisEquals("0"),
		"EXISTS", unackedKey)
	requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
		"WebSocket ACK must remove the HASH payload index", redisEquals("0"),
		"EXISTS", unackedIndexKey)
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
	endpoints := transmiteBVarEndpoints(t)
	before := sumBVar(t, endpoints, "user_info_rpc_total")

	for i := 0; i < 20; i++ {
		sendCacheTestMessage(t, user, convID, fmt.Sprintf("repeat-%d", i))
	}

	after := sumBVar(t, endpoints, "user_info_rpc_total")
	require.LessOrEqual(t, after-before, int64(len(endpoints)))
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", userInfoRedisKey(user.UserID)))
}

func TestFN_CA_UserInfoSingleflight(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	verify.RedisCLI(t, "DEL", userInfoRedisKey(user.UserID))
	endpoints := transmiteBVarEndpoints(t)
	before := sumBVar(t, endpoints, "user_info_rpc_total")

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

	after := sumBVar(t, endpoints, "user_info_rpc_total")
	require.LessOrEqual(t, after-before, int64(len(endpoints)),
		"each Transmite process may originate at most one Identity RPC")
}

func transmiteBVarEndpoints(t testing.TB) []string {
	t.Helper()
	parts := strings.Split(HTTP.Config().Infra.TransmiteVars, ",")
	endpoints := make([]string, 0, len(parts))
	for _, part := range parts {
		endpoint := strings.TrimRight(strings.TrimSpace(part), "/")
		if endpoint == "" {
			t.Fatal("empty Transmite bvar endpoint")
		}
		endpoints = append(endpoints, endpoint)
	}
	return endpoints
}

func sumBVar(t testing.TB, endpoints []string, name string) int64 {
	t.Helper()
	var total int64
	for _, endpoint := range endpoints {
		total += verify.BVar(t, endpoint, name)
	}
	return total
}

func TestFN_CA_UserInfoInvalidatedAfterProfileUpdate(t *testing.T) {
	user, _, convID := fixture.MakeFriends(t, HTTP)
	key := userInfoRedisKey(user.UserID)
	verify.RedisCLI(t, "DEL", key)
	sendCacheTestMessage(t, user, convID, "warm")
	require.Equal(t, "1", verify.RedisCLI(t, "EXISTS", key))
	warmInfo := &common.UserInfo{}
	require.NoError(t, proto.Unmarshal(redisRaw(t, key), warmInfo))
	require.NotEmpty(t, warmInfo.GetNickname())

	newNickname := fmt.Sprintf("cache_%d", time.Now().UnixNano()%1_000_000_000)
	require.NotEqual(t, warmInfo.GetNickname(), newNickname)
	updateRsp := &identity.UpdateProfileRsp{}
	require.NoError(t, user.DoAuth("/service/identity/update_profile", &identity.UpdateProfileReq{
		RequestId: client.NewRequestID(),
		Nickname:  &newNickname,
	}, updateRsp))
	require.True(t, updateRsp.GetHeader().GetSuccess(), updateRsp.GetHeader().GetErrorMessage())
	require.Equal(t, newNickname, updateRsp.GetUserInfo().GetNickname())
	require.Equal(t, "0", verify.RedisCLI(t, "EXISTS", key))

	// UpdateProfile invalidates shared L2 immediately. A Transmite process may
	// still publish its bounded stale L1 value until the documented 45s lifetime
	// expires; this waits at the business boundary instead of forcing an internal
	// generation/CAS interleaving with a production timing hook.
	time.Sleep(55 * time.Second)
	sendCacheTestMessage(t, user, convID, "after-update")
	// The next Transmite lookup must repopulate Redis from Identity with the
	// latest profile, proving stale publication cannot survive the L1 bound.
	serialized := redisRaw(t, key)
	info := &common.UserInfo{}
	require.NoError(t, proto.Unmarshal(serialized, info))
	require.Equal(t, newNickname, info.GetNickname())
	require.NotEqual(t, warmInfo.GetNickname(), info.GetNickname())
}
