//go:build func

package func_test

import (
	"crypto/sha256"
	"fmt"
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	media "chatnow-tests/proto/chatnow/media"
	msg "chatnow-tests/proto/chatnow/message"
	relationship "chatnow-tests/proto/chatnow/relationship"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// FN-CC-01 | P0 | 并发 | 10 goroutine 用相同 client_msg_id 发消息，仅 1 条落库
func TestFN_CC_SendMessage_SameClientMsgId(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	clientMsgID := client.NewRequestID()

	// 10 goroutine 并发用相同 client_msg_id 发消息
	var wg sync.WaitGroup
	successCount := 0
	var mu sync.Mutex
	results := make([]int64, 0, 10)

	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: convID,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "concurrent-same-id"}},
				},
				ClientMsgId: clientMsgID,
			}
			rsp := &transmite.SendMessageRsp{}
			err := alice.DoAuth("/service/transmite/send", req, rsp)
			if err == nil && rsp.Header.Success && rsp.Message != nil {
				mu.Lock()
				successCount++
				results = append(results, rsp.Message.MessageId)
				mu.Unlock()
			}
		}()
	}
	wg.Wait()

	// 至少 1 次成功
	require.GreaterOrEqual(t, successCount, 1, "至少 1 次发送应成功")

	// 所有成功的请求应返回相同 message_id（幂等）
	firstID := results[0]
	for _, id := range results {
		assert.Equal(t, firstID, id, "相同 client_msg_id 应返回相同 message_id")
	}

	// 直查 DB：仅有 1 条消息
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}

// FN-CC-02 | P1 | concurrency | 10 goroutine 并发发消息，全部落库，seq 不重复
func TestFN_CC_SendMessage_DifferentMsgId(t *testing.T) {
	a, _, convID := setupConv(t)

	var wg sync.WaitGroup
	msgIDs := make([]int64, 10)
	seqIDs := make([]uint64, 10)
	errs := make([]error, 10)
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: convID,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: fmt.Sprintf("concurrent-%d", idx)}},
				},
				ClientMsgId: client.NewRequestID(), // 每次不同
			}
			rsp := &transmite.SendMessageRsp{}
			errs[idx] = a.DoAuth("/service/transmite/send", req, rsp)
			if errs[idx] != nil {
				return
			}
			if rsp.Header == nil {
				errs[idx] = fmt.Errorf("send response missing header")
				return
			}
			if !rsp.Header.Success {
				errs[idx] = fmt.Errorf("send failed: code=%d message=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
				return
			}
			if rsp.Message == nil {
				errs[idx] = fmt.Errorf("successful send response missing message")
				return
			}
			msgIDs[idx] = rsp.Message.MessageId
			seqIDs[idx] = rsp.Message.SeqId
		}(i)
	}
	wg.Wait()

	// 验证全部成功
	for i, err := range errs {
		require.NoError(t, err, "goroutine %d failed", i)
		require.NotZero(t, msgIDs[i], "goroutine %d 未返回 message_id", i)
		require.NotZero(t, seqIDs[i], "goroutine %d 未返回 seq_id", i)
	}

	// 验证 message_id 和 seq_id 均不重复
	messageIDSet := make(map[int64]struct{})
	for _, id := range msgIDs {
		_, exists := messageIDSet[id]
		require.False(t, exists, "message_id %d 重复", id)
		messageIDSet[id] = struct{}{}
	}
	seqIDSet := make(map[uint64]struct{})
	for _, seqID := range seqIDs {
		_, exists := seqIDSet[seqID]
		require.False(t, exists, "seq_id %d 重复", seqID)
		seqIDSet[seqID] = struct{}{}
	}

	// 直查 DB：message 表有 10 条
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 10)
}

// FN-CC-03 | P1 | concurrency | 好友通过瞬间并发发消息，不丢
func TestFN_CC_FriendAccept_ThenSend(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// a 发好友申请
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))

	// b 通过 + a 立即并发发消息（会话刚创建）
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	require.True(t, handleRsp.Header.Success, "handle_friend_request 失败: %s", handleRsp.Header.ErrorMessage)
	convID := handleRsp.GetNewConversationId()
	require.NotEmpty(t, convID, "新会话 ID 为空")

	// 并发发 5 条消息
	var wg sync.WaitGroup
	errs := make([]error, 5)
	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			req := &transmite.SendMessageReq{
				RequestId:      client.NewRequestID(),
				ConversationId: convID,
				Content: &msg.MessageContent{
					Type: msg.MessageType_TEXT,
					Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "race-msg"}},
				},
				ClientMsgId: client.NewRequestID(),
			}
			rsp := &transmite.SendMessageRsp{}
			errs[idx] = a.DoAuth("/service/transmite/send", req, rsp)
			if errs[idx] == nil && !rsp.Header.Success {
				errs[idx] = fmt.Errorf("send failed: %s", rsp.Header.ErrorMessage)
			}
		}(i)
	}
	wg.Wait()

	// 验证全部发送成功
	for i, err := range errs {
		require.NoError(t, err, "goroutine %d 发送失败", i)
	}

	// 直查 DB：5 条消息全部落库
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 5)
}

// FN-CC-04 | P1 | concurrency | 相同 content_hash 并发上传，dedup 正确
func TestFN_CC_MediaUpload_SameHash(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("cc-media-same-hash")
	hash := sha256.Sum256(content)
	hashStr := fmt.Sprintf("sha256:%x", hash)

	// 先完整上传一次，确保 dedup 记录存在
	existingID := fixture.UploadFile(t, authed, content, "text/plain")
	require.NotEmpty(t, existingID)

	// 5 goroutine 并发 ApplyUpload 相同 hash（已存在）
	var wg sync.WaitGroup
	fileIDs := make([]string, 5)
	errs := make([]error, 5)
	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			req := &media.ApplyUploadReq{
				RequestId:   client.NewRequestID(),
				FileName:    "cc-dup.bin",
				FileSize:    int64(len(content)),
				MimeType:    "text/plain",
				ContentHash: hashStr,
				Purpose:     media.MediaPurpose_CHAT,
			}
			rsp := &media.ApplyUploadRsp{}
			errs[idx] = authed.DoAuth("/service/media/apply_upload", req, rsp)
			if errs[idx] == nil && rsp.Header.Success {
				fileIDs[idx] = rsp.FileId
			}
		}(i)
	}
	wg.Wait()

	// 验证全部成功
	for i, err := range errs {
		require.NoError(t, err, "goroutine %d 失败", i)
		require.NotEmpty(t, fileIDs[i], "goroutine %d 的 file_id 为空", i)
	}

	// 验证所有返回的 file_id 相同（dedup 正确）
	require.Equal(t, existingID, fileIDs[0], "dedup 应返回已存在的 file_id")
	for i, id := range fileIDs {
		assert.Equal(t, fileIDs[0], id, "goroutine %d 的 file_id 应一致（dedup）", i)
	}
}

// FN-CC-05 | P2 | concurrency | 多用户同时给同一消息加相同 emoji
func TestFN_CC_Reaction_SameEmoji(t *testing.T) {
	a, b, convID := setupConv(t)
	mID, _ := sendMsg(t, a, convID, "react-concurrent")

	// 单聊只有 2 人，此处用 a 和 b 各加相同 emoji 测试幂等
	reactioners := make([]*client.HTTPClient, 0, 2)
	reactioners = append(reactioners, a) // a 也加 reaction
	reactioners = append(reactioners, b)

	var wg sync.WaitGroup
	emoji := "👍"
	errs := make([]error, len(reactioners))
	responses := make([]*msg.AddReactionRsp, len(reactioners))
	for i, u := range reactioners {
		wg.Add(1)
		go func(idx int, user *client.HTTPClient) {
			defer wg.Done()
			req := &msg.AddReactionReq{
				RequestId: client.NewRequestID(),
				MessageId: mID,
				Emoji:     emoji,
			}
			rsp := &msg.AddReactionRsp{}
			responses[idx] = rsp
			errs[idx] = user.DoAuth("/service/message/add_reaction", req, rsp)
		}(i, u)
	}
	wg.Wait()

	// 两个用户的 reaction 都应成功
	for i, err := range errs {
		require.NoError(t, err, "goroutine %d add_reaction 出错", i)
		require.NotNil(t, responses[i], "goroutine %d response 为空", i)
		require.NotNil(t, responses[i].Header, "goroutine %d response header 为空", i)
		require.True(t, responses[i].Header.Success, "goroutine %d add_reaction 失败: %s", i,
			responses[i].Header.ErrorMessage)
	}

	// 验证同一 emoji 聚合为一组，包含两个用户
	getReq := &msg.GetReactionsReq{RequestId: client.NewRequestID(), MessageId: mID}
	getRsp := &msg.GetReactionsRsp{}
	require.NoError(t, a.DoAuth("/service/message/get_reactions", getReq, getRsp))
	require.NotNil(t, getRsp.Header)
	require.True(t, getRsp.Header.Success, "get_reactions 失败: %s", getRsp.Header.ErrorMessage)
	require.Len(t, getRsp.Reactions, 1, "目标 emoji 应仅有一个 reaction group")
	reaction := getRsp.Reactions[0]
	require.NotNil(t, reaction)
	require.Equal(t, emoji, reaction.Emoji)
	require.Equal(t, int32(2), reaction.Count)
	require.ElementsMatch(t, []string{a.UserID, b.UserID}, reaction.RecentUserIds)
	require.True(t, reaction.SelfReacted, "用户 a 查询时应标记 self_reacted")
}
