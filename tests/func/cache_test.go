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
	"path/filepath"
	"regexp"
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
		{"Codes", "class Codes", "class Seq"},
		{"DeviceSet", "class DeviceSet", "class ReadAck"},
		{"OnlineRoute", "class OnlineRoute", "class RateLimiter"},
		{"UnackedPush", "class UnackedPush", "class PresenceRedis"},
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
		{"Codes", []string{
			"_c->set(key::kVerifyCode + cid, code, randomized_ttl(ttl))",
		}},
		{"DeviceSet", []string{
			"randomized_ttl(kSessionTtl)",
			"key::device_set_key(uid)",
			"_c->eval<long long>(kAddLua",
			"redis.call('SADD', KEYS[1], ARGV[1])",
			"redis.call('EXPIRE', KEYS[1], ttl)",
			"key::legacy_device_set_key(uid)",
			"kLegacyGraceTtl",
		}},
		{"OnlineRoute", []string{
			"key::online_key(uid), key::device_set_key(uid)",
			"_c->eval<long long>(kBindLua",
			"_c->eval<long long>(kTouchLua",
			"_c->eval<long long>(kUnbindLua",
			"redis.call('EXPIRE', KEYS[1], route_ttl)",
			"redis.call('EXPIRE', KEYS[2], device_ttl)",
		}},
		{"UnackedPush", []string{
			"_c->eval<long long>(kPushLua",
			"_c->eval<long long>(kBumpScoreLua",
			"_c->eval<long long>(kAckLua",
			"redis.call('ZADD', KEYS[1]",
			"redis.call('HSET', KEYS[2]",
			"redis.call('EXPIRE', KEYS[1], ttl)",
			"redis.call('EXPIRE', KEYS[2], ttl)",
		}},
	}
	for _, rule := range rules {
		if err := requireSourceFragments(rule.name, sections[rule.name], rule.fragments...); err != nil {
			return err
		}
	}

	fixedTTL := regexp.MustCompile(`_c->(?:set|expire)\([^;]*, ttl\)`)
	for _, name := range []string{"Session", "Status", "Codes"} {
		if fixedTTL.MatchString(sections[name]) {
			return fmt.Errorf("%s contains a direct fixed TTL cache write", name)
		}
	}
	if strings.Count(sections["UnackedPush"], "randomized_ttl(ttl)") != 2 {
		return fmt.Errorf("UnackedPush push and bump_score must each sample one randomized TTL")
	}
	if strings.Contains(sections["UnackedPush"], "_c->zadd(") ||
		strings.Contains(sections["UnackedPush"], "_c->hset(") {
		return fmt.Errorf("UnackedPush bypasses its atomic scripts")
	}
	return nil
}

func TestFN_CA_ResilienceSourceContracts(t *testing.T) {
	dao := readRepoSource(t, "common/dao/data_redis.hpp")
	transmiteSource := readRepoSource(t, "transmite/source/transmite_server.h")
	pushSource := readRepoSource(t, "push/source/push_server.h")
	breaker := readRepoSource(t, "common/utils/redis_circuit_breaker.hpp")
	for _, contract := range []struct {
		name   string
		source string
		want   []string
	}{
		{"generation fence", dao, []string{"set_if_generation", "kSetIfGenerationLua", "redis.call('INCR', KEYS[2])"}},
		{"fenced fill", transmiteSource, []string{"generation(uid)", "set_if_generation(uid, bytes", "set_if_generation(uid, \"\""}},
		{"truth source push", pushSource, []string{"Persist before delivery", "ConsumeAction::NackRequeue", "unacked persistence unavailable"}},
		{"atomic ack", dao, []string{"kAckLua", "redis.call('ZREM', KEYS[1]", "redis.call('HDEL', KEYS[2]"}},
		{"atomic breaker hot path", breaker, []string{"std::atomic<uint64_t> _generation", "std::atomic<uint32_t> _consecutive_failures"}},
	} {
		for _, want := range contract.want {
			if !strings.Contains(contract.source, want) {
				t.Errorf("%s missing %q", contract.name, want)
			}
		}
	}
}

func requireCacheTTLSourceContracts(t testing.TB, source string) {
	t.Helper()
	require.NoError(t, checkCacheTTLSourceContracts(source))
}

func TestFN_CA_TTLJitterSourceContract(t *testing.T) {
	daoSource := readRepoSource(t, "common/dao/data_redis.hpp")
	requireCacheTTLSourceContracts(t, daoSource)

	t.Run("rejects fixed Session TTL", func(t *testing.T) {
		mutant := strings.Replace(daoSource, "randomized_ttl(ttl)", "ttl", 1)
		require.Error(t, checkCacheTTLSourceContracts(mutant))
	})
	t.Run("rejects non-atomic DeviceSet add", func(t *testing.T) {
		mutant := strings.Replace(daoSource, "_c->eval<long long>(kAddLua", "_c->sadd", 1)
		require.Error(t, checkCacheTTLSourceContracts(mutant))
	})
	t.Run("rejects non-atomic UnackedPush write", func(t *testing.T) {
		mutant := strings.Replace(daoSource, "_c->eval<long long>(kPushLua", "_c->zadd", 1)
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
			requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
				"DeviceSet key must exist", redisEquals("1"), "EXISTS", deviceKey)
			if i == 0 {
				verify.RedisCLI(t, "EXPIRE", deviceKey, "60")
				heartbeatBefore = verify.RedisTTL(t, deviceKey)
				ws.writeBinary(t, cacheTestHeartbeatNotify(user.UserID))
				requireEventuallyRedis(t, 5*time.Second, 50*time.Millisecond,
					"heartbeat must renew DeviceSet TTL", func(result string) (bool, error) {
						seconds, err := strconv.ParseInt(result, 10, 64)
						return time.Duration(seconds)*time.Second > heartbeatBefore+24*time.Hour, err
					}, "TTL", deviceKey)
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
