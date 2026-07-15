//go:build func

package func_test

import (
	"bytes"
	"fmt"
	"os/exec"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
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
