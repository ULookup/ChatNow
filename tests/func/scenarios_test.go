//go:build func

package func_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	"chatnow-tests/pkg/verify"
	common "chatnow-tests/proto/chatnow/common"
	conversation "chatnow-tests/proto/chatnow/conversation"
	identity "chatnow-tests/proto/chatnow/identity"
	media "chatnow-tests/proto/chatnow/media"
	msg "chatnow-tests/proto/chatnow/message"
	push "chatnow-tests/proto/chatnow/push"
	relationship "chatnow-tests/proto/chatnow/relationship"
	transmite "chatnow-tests/proto/chatnow/transmite"
)

// ---------------------------------------------------------------------------
// Scenario 1: Register → Login → Add Friend → Accept → Send First Message → Sync → GetHistory
// ---------------------------------------------------------------------------

func TestScenario_RegisterToFirstMessage(t *testing.T) {
	// Step 1: Register Alice and Bob
	alice, aliceUser, alicePwd := fixture.RegisterAndLogin(t, HTTP)
	bob, bobUser, bobPwd := fixture.RegisterAndLogin(t, HTTP)
	_, _, _, _ = aliceUser, alicePwd, bobUser, bobPwd

	// Step 2: Alice searches for Bob (by user ID prefix)
	searchReq := &identity.SearchUsersReq{RequestId: client.NewRequestID(), SearchKey: bob.UserID[:4]}
	searchRsp := &identity.SearchUsersRsp{}
	require.NoError(t, alice.DoAuth("/service/identity/search_users", searchReq, searchRsp))
	assert.True(t, searchRsp.Header.Success)

	// Step 3: Alice sends friend request to Bob
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: bob.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	assert.True(t, sendRsp.Header.Success)

	// Step 4: Bob accepts
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   alice.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, bob.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	assert.True(t, handleRsp.Header.Success)
	convID := handleRsp.GetNewConversationId()
	assert.NotEmpty(t, convID)

	// Step 5: Alice sends first message
	msgText := "Hello Bob!"
	sendMsgReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: msgText}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendMsgRsp := &transmite.SendMessageRsp{}
	require.NoError(t, alice.DoAuth("/service/transmite/send", sendMsgReq, sendMsgRsp))
	assert.True(t, sendMsgRsp.Header.Success)

	// Step 6: Bob syncs messages
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          20,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
	assert.True(t, syncRsp.Header.Success)
	assert.NotEmpty(t, syncRsp.GetMessages())

	// Step 7: Bob gets history
	histReq := &msg.GetHistoryReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		BeforeSeq:      syncRsp.GetLatestSeq() + 1,
		Limit:          20,
	}
	histRsp := &msg.GetHistoryRsp{}
	require.NoError(t, bob.DoAuth("/service/message/get_history", histReq, histRsp))
	assert.True(t, histRsp.Header.Success)
	assert.NotEmpty(t, histRsp.GetMessages())
}

// ---------------------------------------------------------------------------
// Scenario 2: Group Chat Lifecycle
// ---------------------------------------------------------------------------

func TestScenario_GroupChatLifecycle(t *testing.T) {
	// Create owner + 2 members
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	m2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// Create group
	name := "test-group-scenario"
	createReq := &conversation.CreateConversationReq{
		RequestId: client.NewRequestID(),
		Type:      conversation.ConversationType_GROUP,
		Name:      &name,
		MemberIds: []string{m1.UserID, m2.UserID},
	}
	createRsp := &conversation.CreateConversationRsp{}
	require.NoError(t, owner.DoAuth("/service/conversation/create", createReq, createRsp))
	convID := createRsp.Conversation.ConversationId

	// Send image message with @mention
	sendReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_IMAGE,
			Body: &msg.MessageContent_Image{Image: &msg.ImageContent{
				FileId:       "fake-img",
				Width:        100,
				Height:       100,
				ThumbnailUrl: "http://x.com/t.jpg",
			}},
		},
		ClientMsgId:      client.NewRequestID(),
		MentionedUserIds: []string{m1.UserID},
	}
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, owner.DoAuth("/service/transmite/send", sendReq, sendRsp))
	msgID := sendRsp.Message.MessageId

	// Add reaction
	reactReq := &msg.AddReactionReq{RequestId: client.NewRequestID(), MessageId: msgID, Emoji: "🔥"}
	reactRsp := &msg.AddReactionRsp{}
	require.NoError(t, m1.DoAuth("/service/message/add_reaction", reactReq, reactRsp))
	assert.True(t, reactRsp.Header.Success)

	// Recall message
	recallReq := &msg.RecallMessageReq{RequestId: client.NewRequestID(), ConversationId: convID, MessageId: msgID}
	recallRsp := &msg.RecallMessageRsp{}
	require.NoError(t, owner.DoAuth("/service/message/recall", recallReq, recallRsp))
	assert.True(t, recallRsp.Header.Success)

	// Dismiss group
	dismissReq := &conversation.DismissConversationReq{RequestId: client.NewRequestID(), ConversationId: convID}
	dismissRsp := &conversation.DismissConversationRsp{}
	require.NoError(t, owner.DoAuth("/service/conversation/dismiss", dismissReq, dismissRsp))
	assert.True(t, dismissRsp.Header.Success)
}

// ---------------------------------------------------------------------------
// Scenario 3: Friend Full Lifecycle
// ---------------------------------------------------------------------------

func TestScenario_FriendFullLifecycle(t *testing.T) {
	a, _, _ := fixture.RegisterAndLogin(t, HTTP)
	b, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// A sends friend request to B — B accepts
	sendReq := &relationship.SendFriendReq{RequestId: client.NewRequestID(), RespondentId: b.UserID}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   a.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, b.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	convID := handleRsp.GetNewConversationId()
	assert.NotEmpty(t, convID)

	// Exchange messages
	sendMsgReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "hey"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendMsgRsp := &transmite.SendMessageRsp{}
	require.NoError(t, a.DoAuth("/service/transmite/send", sendMsgReq, sendMsgRsp))
	assert.True(t, sendMsgRsp.Header.Success)

	// Remove friend
	removeReq := &relationship.RemoveFriendReq{RequestId: client.NewRequestID(), PeerId: b.UserID}
	removeRsp := &relationship.RemoveFriendRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/remove_friend", removeReq, removeRsp))
	assert.True(t, removeRsp.Header.Success)

	// Verify friend list empty
	listReq := &relationship.ListFriendsReq{RequestId: client.NewRequestID(), Page: &common.PageRequest{Limit: 20}}
	listRsp := &relationship.ListFriendsRsp{}
	require.NoError(t, a.DoAuth("/service/relationship/list_friends", listReq, listRsp))
	assert.Empty(t, listRsp.FriendList)
}

// ---------------------------------------------------------------------------
// Scenario 4: Offline Message Sync（离线消息同步）
// SC-04 | P0 | u2 离线 -> u1 发 3 条 -> u2 上线 sync -> WS 实时推送
// ---------------------------------------------------------------------------

func TestScenario_OfflineMessageSync(t *testing.T) {
	alice, _, _ := fixture.RegisterAndLogin(t, HTTP)
	bob, bobUser, bobPwd := fixture.RegisterAndLogin(t, HTTP)

	// Step 1: bob 登出（模拟离线）
	logoutReq := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bob.DoAuth("/service/identity/logout", logoutReq, &identity.LogoutRsp{}))

	// Step 2: alice 发好友申请 -> bob 重新登录后处理
	// 注：bob 已登出，需要重新登录后才能接受好友申请
	// 改为：先加好友，再登出
	_ = bobUser
	_ = bobPwd

	// 重新设计：先加好友，再登出
	bobRelogin := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// alice 发好友申请
	sendReq := &relationship.SendFriendReq{
		RequestId:    client.NewRequestID(),
		RespondentId: bobRelogin.UserID,
	}
	sendRsp := &relationship.SendFriendRsp{}
	require.NoError(t, alice.DoAuth("/service/relationship/send_friend_request", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)

	// bob 接受
	handleReq := &relationship.HandleFriendReq{
		RequestId:     client.NewRequestID(),
		NotifyEventId: sendRsp.GetNotifyEventId(),
		Agree:         true,
		ApplyUserId:   alice.UserID,
	}
	handleRsp := &relationship.HandleFriendRsp{}
	require.NoError(t, bobRelogin.DoAuth("/service/relationship/handle_friend_request", handleReq, handleRsp))
	require.True(t, handleRsp.Header.Success)
	convID := handleRsp.GetNewConversationId()
	require.NotEmpty(t, convID)

	// bob 登出（模拟离线）
	logoutReq2 := &identity.LogoutReq{RequestId: client.NewRequestID()}
	require.NoError(t, bobRelogin.DoAuth("/service/identity/logout", logoutReq2, &identity.LogoutRsp{}))

	// Step 3: alice 发 3 条消息（bob 离线）
	texts := []string{"offline-1", "offline-2", "offline-3"}
	var lastSeq uint64
	for _, txt := range texts {
		_, seq := fixture.SendTextMessage(t, alice, convID, txt)
		lastSeq = seq
	}

	// Step 4: bob 重新登录
	bobOnline := fixture.LoginUser(t, HTTP, bobUser, bobPwd)

	// Step 5: bob sync，验证 3 条按序到达
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          20,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 3, "应返回 3 条离线消息")

	for i, m := range syncRsp.Messages {
		assert.Equal(t, texts[i], m.GetContent().GetText().Text, "第 %d 条消息内容不匹配", i+1)
		if i > 0 {
			assert.Less(t, syncRsp.Messages[i-1].SeqId, m.SeqId, "seq 应递增")
		}
	}

	// Step 6: bob 开 WS，不应收到旧消息推送（已通过 sync 拉取）
	wsBob := fixture.ConnectWS(t, bobOnline)
	defer wsBob.Close()
	time.Sleep(1 * time.Second) // 等 WS 鉴权完成

	// Step 7: alice 再发 1 条，bob WS 应收到实时推送
	fixture.SendTextMessage(t, alice, convID, "realtime-msg")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	notify, err := wsBob.WaitForNotify(ctx, int32(push.NotifyType_CHAT_MESSAGE_NOTIFY))
	require.NoError(t, err, "应收到实时消息推送")
	actualMsg := notify.GetNewMessageInfo().GetMessageInfo()
	if actualMsg != nil {
		assert.Equal(t, "realtime-msg", actualMsg.GetContent().GetText().Text)
	}

	// Step 8: bob 增量 sync，仅返回新 1 条
	syncReq2 := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       lastSeq,
		Limit:          20,
	}
	syncRsp2 := &msg.SyncMessagesRsp{}
	require.NoError(t, bobOnline.DoAuth("/service/message/sync", syncReq2, syncRsp2))
	require.True(t, syncRsp2.Header.Success)
	require.Len(t, syncRsp2.Messages, 1, "增量 sync 应仅返回 1 条新消息")

	// Step 9: 数据一致性 - 直查 DB
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 4) // 3 离线 + 1 实时 = 4
}

// ---------------------------------------------------------------------------
// Scenario 5: Media Upload Full Flow（媒体三步上传全链路）
// SC-05 | P0 | scenario | 媒体三步上传全链路：apply->PUT->complete->download->dedup->multipart
// ---------------------------------------------------------------------------

func TestScenario_MediaUploadFullFlow(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("sc05-media-full-flow-content")
	hash := sha256.Sum256(content)
	hashStr := fmt.Sprintf("sha256:%x", hash)

	// Step 1: ApplyUpload
	applyReq := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "sc05.txt",
		FileSize: int64(len(content)), MimeType: "text/plain",
		ContentHash: hashStr, Purpose: media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	require.NoError(t, user.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
	require.True(t, applyRsp.Header.Success)
	fileID := applyRsp.FileId
	require.NotEmpty(t, fileID)

	// Step 2: PUT 到 MinIO presigned URL
	httpReq, _ := http.NewRequest("PUT", applyRsp.UploadUrl, bytes.NewReader(content))
	if applyRsp.Headers != nil {
		for k, v := range applyRsp.Headers {
			httpReq.Header.Set(k, v)
		}
	}
	putResp, err := http.DefaultClient.Do(httpReq)
	require.NoError(t, err)
	require.Equal(t, 200, putResp.StatusCode)
	putResp.Body.Close()

	// Step 3: CompleteUpload
	completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: fileID}
	completeRsp := &media.CompleteUploadRsp{}
	require.NoError(t, user.DoAuth("/service/media/complete_upload", completeReq, completeRsp))
	require.True(t, completeRsp.Header.Success)

	// Step 4: ApplyDownload + 下载验证内容
	dlReq := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: fileID}
	dlRsp := &media.ApplyDownloadRsp{}
	require.NoError(t, user.DoAuth("/service/media/apply_download", dlReq, dlRsp))
	require.True(t, dlRsp.Header.Success)
	dlResp, err := http.Get(dlRsp.DownloadUrl)
	require.NoError(t, err)
	body, _ := io.ReadAll(dlResp.Body)
	dlResp.Body.Close()
	assert.Equal(t, content, body, "下载内容与上传不一致")

	// Step 5: 重复 ApplyUpload（相同 hash）-> dedup 返回相同 file_id
	applyReq2 := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "sc05-dup.txt",
		FileSize: int64(len(content)), MimeType: "text/plain",
		ContentHash: hashStr, Purpose: media.MediaPurpose_CHAT,
	}
	applyRsp2 := &media.ApplyUploadRsp{}
	require.NoError(t, user.DoAuth("/service/media/apply_upload", applyReq2, applyRsp2))
	require.True(t, applyRsp2.Header.Success)
	assert.True(t, applyRsp2.AlreadyExists, "相同 hash 应返回 already_exists=true")
	assert.Equal(t, fileID, applyRsp2.FileId, "dedup 应返回相同 file_id")

	// Step 6: 大文件 multipart（6MB -> 3 parts @ 2MB）
	bigContent := make([]byte, 6*1024*1024)
	for i := range bigContent {
		bigContent[i] = byte(i % 256)
	}
	bigFileID := fixture.UploadLargeFile(t, user, bigContent, "application/octet-stream", 2*1024*1024)
	require.NotEmpty(t, bigFileID)

	// 下载大文件验证
	bigDlReq := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: bigFileID}
	bigDlRsp := &media.ApplyDownloadRsp{}
	require.NoError(t, user.DoAuth("/service/media/apply_download", bigDlReq, bigDlRsp))
	require.True(t, bigDlRsp.Header.Success)
	bigResp, err := http.Get(bigDlRsp.DownloadUrl)
	require.NoError(t, err)
	bigBody, _ := io.ReadAll(bigResp.Body)
	bigResp.Body.Close()
	assert.Equal(t, bigContent, bigBody, "大文件下载内容不一致")

	// Step 7: 数据一致性 - GetFileInfo 验证
	infoReq := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: fileID}
	infoRsp := &media.GetFileInfoRsp{}
	require.NoError(t, user.DoAuth("/service/media/get_file_info", infoReq, infoRsp))
	require.True(t, infoRsp.Header.Success)
	assert.Equal(t, int64(len(content)), infoRsp.FileInfo.FileSize)

	// Step 8: 数据一致性 - 直查 DB quota
	// 注：dedup 命中不增加 quota，故 used_bytes = len(content) + len(bigContent)
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	time.Sleep(1 * time.Second) // 等待 DB 异步写入
	dbV.MediaQuota(t, user.UserID, int64(len(content)+len(bigContent)))
}

// ---------------------------------------------------------------------------
// Scenario 6: Message Reliability（MQ 可用版本）
// SC-06 | P0 | client_msg_id 幂等 + 消息不丢不重
// 注：Phase 1 不做 MQ stop/start（那是 RL-01 的职责），
// 此版本验证 MQ 正常可用时的 client_msg_id 幂等机制。
// ---------------------------------------------------------------------------

func TestScenario_MessageReliability(t *testing.T) {
	alice, bob, convID := fixture.MakeFriends(t, HTTP)
	_ = bob

	// Step 1: alice 发消息，获得 message_id
	clientMsgID := client.NewRequestID()
	msgID1, seq1, success1 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)
	require.True(t, success1, "第一次发送应成功")
	require.NotZero(t, msgID1)

	// Step 2: 用相同 client_msg_id 重发（模拟网络重传）
	msgID2, seq2, success2 := fixture.SendTextMessageWithClientMsgId(t, alice, convID, "reliability-test", clientMsgID)

	// 幂等验证
	if success2 {
		assert.Equal(t, msgID1, msgID2, "相同 client_msg_id 应返回相同 message_id")
		assert.Equal(t, seq1, seq2, "相同 client_msg_id 应返回相同 seq_id")
	}

	// Step 3: bob sync 验证收到该消息（仅 1 条）
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, bob.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.True(t, syncRsp.Header.Success)
	require.Len(t, syncRsp.Messages, 1, "应仅收到 1 条消息（幂等去重）")
	assert.Equal(t, msgID1, syncRsp.Messages[0].MessageId)
	assert.Equal(t, "reliability-test", syncRsp.Messages[0].GetContent().GetText().Text)

	// Step 4: SelectByClientMsgId 验证可查到
	selectReq := &msg.SelectByClientMsgIdReq{
		RequestId:   client.NewRequestID(),
		ClientMsgId: clientMsgID,
	}
	selectRsp := &msg.SelectByClientMsgIdRsp{}
	require.NoError(t, alice.DoAuth("/service/message/select_by_client_msg_id", selectReq, selectRsp))
	require.True(t, selectRsp.Header.Success)
	require.NotNil(t, selectRsp.Message)
	assert.Equal(t, msgID1, selectRsp.Message.MessageId)

	// Step 5: 数据一致性 - DB 仅 1 条（不重复）
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
	dbV.MessageByClientMsgId(t, clientMsgID, true)
}

// ---------------------------------------------------------------------------
// Scenario 7: Multi-Device Login Kick
// SC-07 | P1 | scenario | 多设备登录：设备 A 登录 -> 设备 B 登录 -> A 被踢 -> A token 失效
// ---------------------------------------------------------------------------

func TestScenario_MultiDeviceLogin(t *testing.T) {
	// 先注册用户（LoginUser 要求用户已存在）
	username := "sc07_user_" + client.NewRequestID()[:8]
	password := "Sc07@123456"

	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		Nickname: username,
	}
	require.NoError(t, HTTP.DoNoAuth("/service/identity/register", regReq, &identity.RegisterRsp{}))

	// 设备 A 登录
	deviceA := fixture.LoginUser(t, HTTP, username, password)
	require.NotEmpty(t, deviceA.AccessToken)

	// 验证 A 能调 API
	profileReq := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	require.NoError(t, deviceA.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}))

	// 设备 B 登录同用户
	deviceB := fixture.LoginUser(t, HTTP, username, password)
	require.NotEmpty(t, deviceB.AccessToken)
	require.NotEqual(t, deviceA.AccessToken, deviceB.AccessToken, "B 的 token 应不同于 A")

	// 设备 A 的 token 应失效（被踢）
	err := deviceA.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{})
	assert.Error(t, err, "设备 A 被踢后 token 应失效")

	// 设备 B 仍可调 API
	require.NoError(t, deviceB.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}))
}

// ---------------------------------------------------------------------------
// Scenario 8: Large Group Fan-Out (Read Diffusion)
// SC-08 | P1 | scenario | 200+ 成员群发消息，验证读扩散（仅写主表，各成员 sync 收到）
// ---------------------------------------------------------------------------

func TestScenario_LargeGroupFanOut(t *testing.T) {
	owner, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 批量注册 200 成员（分批避免单次请求过大）
	members := make([]*client.HTTPClient, 0, 200)
	for i := 0; i < 200; i++ {
		m, _, _ := fixture.RegisterAndLogin(t, HTTP)
		members = append(members, m)
	}

	// 建群（200 成员 + owner = 201）
	convID := fixture.CreateGroupWithMembers(t, owner, members, "sc08-large-group-200")

	// owner 发消息
	sendReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "sc08-large-group-msg"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, owner.DoAuth("/service/transmite/send", sendReq, sendRsp))
	require.True(t, sendRsp.Header.Success)
	msgID := sendRsp.Message.MessageId

	// 抽样 10 个成员验证 sync 收到
	for i := 0; i < 10; i++ {
		idx := i * 20 // 每隔 20 个抽一个
		syncReq := &msg.SyncMessagesReq{
			RequestId:      client.NewRequestID(),
			ConversationId: convID,
			AfterSeq:       0,
			Limit:          10,
		}
		syncRsp := &msg.SyncMessagesRsp{}
		require.NoError(t, members[idx].DoAuth("/service/message/sync", syncReq, syncRsp),
			"成员 %d sync 失败", idx)
		require.NotEmpty(t, syncRsp.Messages, "成员 %d 应收到消息", idx)
		assert.Equal(t, msgID, syncRsp.Messages[0].MessageId, "成员 %d 收到的 message_id 不符", idx)
	}

	// 数据一致性 - 读扩散：message 表仅 1 条
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageCount(t, convID, 1)
}

// ---------------------------------------------------------------------------
// Scenario 9: Unread Count Consistency（未读数跨服务一致性）
// SC-09 | P0 | scenario | 未读数跨服务跨设备一致：发消息 unread+1 -> UpdateReadAck -> unread=0
// ---------------------------------------------------------------------------

func TestScenario_UnreadCountConsistency(t *testing.T) {
	a, b, convID := setupConv(t) // MakeFriends

	// Step 1: a 发 3 条消息
	var lastSeq uint64
	for i := 0; i < 3; i++ {
		_, lastSeq = sendMsg(t, a, convID, "sc09-unread-"+string(rune('0'+i)))
	}

	// Step 2: b ListConversations，验证 unread_count=3
	listReq := &conversation.ListConversationsReq{RequestId: client.NewRequestID()}
	listRsp := &conversation.ListConversationsRsp{}
	require.NoError(t, b.DoAuth("/service/conversation/list", listReq, listRsp))
	var bobConv *conversation.Conversation
	for _, c := range listRsp.Conversations {
		if c.ConversationId == convID {
			bobConv = c
			break
		}
	}
	require.NotNil(t, bobConv, "b 的会话列表中应包含 convID")
	assert.Equal(t, uint64(3), bobConv.Self.UnreadCount, "b 未读数应为 3")

	// Step 3: 数据一致性 - DB unread_count=3
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.UnreadCount(t, b.UserID, convID, 3)

	// Step 4: b UpdateReadAck（读到最后一条 seq）
	ackReq := &msg.UpdateReadAckReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		SeqId:          lastSeq,
	}
	require.NoError(t, b.DoAuth("/service/message/update_read_ack", ackReq, &msg.UpdateReadAckRsp{}))

	// Step 5: b 再次 ListConversations，unread_count=0
	listRsp2 := &conversation.ListConversationsRsp{}
	require.NoError(t, b.DoAuth("/service/conversation/list", listReq, listRsp2))
	for _, c := range listRsp2.Conversations {
		if c.ConversationId == convID {
			assert.Equal(t, uint64(0), c.Self.UnreadCount, "read ack 后未读数应清零")
		}
	}

	// Step 6: 数据一致性 - DB unread_count=0
	dbV.UnreadCount(t, b.UserID, convID, 0)
}

// ---------------------------------------------------------------------------
// Scenario 10: Message Recall Visibility（撤回消息可见性）
// SC-10 | P1 | scenario | 撤回可见性跨设备一致：发消息 -> sync 看到 -> 撤回 -> 另一设备 sync 看到 recalled
// ---------------------------------------------------------------------------

func TestScenario_MessageRecallVisibility(t *testing.T) {
	a, b, convID := setupConv(t)

	// a 发消息
	sendReq := &transmite.SendMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		Content: &msg.MessageContent{
			Type: msg.MessageType_TEXT,
			Body: &msg.MessageContent_Text{Text: &msg.TextContent{Text: "sc10-will-recall"}},
		},
		ClientMsgId: client.NewRequestID(),
	}
	sendRsp := &transmite.SendMessageRsp{}
	require.NoError(t, a.DoAuth("/service/transmite/send", sendReq, sendRsp))
	msgID := sendRsp.Message.MessageId

	// b sync，看到消息内容，status=NORMAL
	syncReq := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp := &msg.SyncMessagesRsp{}
	require.NoError(t, b.DoAuth("/service/message/sync", syncReq, syncRsp))
	require.NotEmpty(t, syncRsp.Messages)
	assert.Equal(t, "sc10-will-recall", syncRsp.Messages[0].GetContent().GetText().Text)
	assert.Equal(t, msg.MessageStatus_MESSAGE_STATUS_NORMAL, syncRsp.Messages[0].Status)

	// a 撤回
	recallReq := &msg.RecallMessageReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		MessageId:      msgID,
	}
	require.NoError(t, a.DoAuth("/service/message/recall", recallReq, &msg.RecallMessageRsp{}))

	// b 再次 sync，看到 status=RECALLED
	syncReq2 := &msg.SyncMessagesReq{
		RequestId:      client.NewRequestID(),
		ConversationId: convID,
		AfterSeq:       0,
		Limit:          10,
	}
	syncRsp2 := &msg.SyncMessagesRsp{}
	require.NoError(t, b.DoAuth("/service/message/sync", syncReq2, syncRsp2))
	require.NotEmpty(t, syncRsp2.Messages)
	assert.Equal(t, msg.MessageStatus_MESSAGE_STATUS_RECALLED, syncRsp2.Messages[0].Status,
		"撤回后 status 应为 RECALLED")

	// 数据一致性 - DB message.status=RECALLED(1)
	time.Sleep(1 * time.Second) // 等待 DB 异步写入
	dbV := verify.NewDBVerifier(Cfg.Database.MySQLDSN)
	defer dbV.Close()
	dbV.MessageStatus(t, msgID, 1)
}

// ---------------------------------------------------------------------------
// Scenario 11: Token Refresh Flow（Token 刷新链路）
// SC-11 | P1 | scenario | token 刷新链路：篡改 token 失败 -> RefreshToken -> 新 token 可用
// ---------------------------------------------------------------------------

func TestScenario_TokenRefreshFlow(t *testing.T) {
	user, _, _ := fixture.RegisterAndLogin(t, HTTP)
	validToken := user.AccessToken
	refreshToken := user.RefreshToken

	// Step 1: 篡改 access_token，调 API 失败
	user.AccessToken = "tampered.invalid.token.payload"
	profileReq := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	err := user.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{})
	assert.Error(t, err, "篡改 token 后应鉴权失败")

	// Step 2: 用 refresh_token 刷新
	refreshReq := &identity.RefreshTokenReq{
		RequestId: client.NewRequestID(), RefreshToken: refreshToken,
	}
	refreshRsp := &identity.RefreshTokenRsp{}
	require.NoError(t, user.DoNoAuth("/service/identity/refresh_token", refreshReq, refreshRsp))
	require.True(t, refreshRsp.Header.Success)
	require.NotEmpty(t, refreshRsp.Tokens.AccessToken)
	require.NotEqual(t, validToken, refreshRsp.Tokens.AccessToken, "新 token 应不同于旧 token")

	// Step 3: 新 token 调 API 成功
	user.AccessToken = refreshRsp.Tokens.AccessToken
	require.NoError(t, user.DoAuth("/service/identity/get_profile", profileReq, &identity.GetProfileRsp{}),
		"新 token 应能调 API")
}
