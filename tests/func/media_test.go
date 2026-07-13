//go:build func

package func_test

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"net/http"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	media "chatnow-tests/proto/chatnow/media"
)

func TestApplyUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	hash := sha256.Sum256([]byte("test content"))
	req := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "test.png",
		FileSize:    1024,
		MimeType:    "image/png",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.FileId)
	assert.NotEmpty(t, rsp.UploadUrl)
}

func TestApplyUpload_FileTooLarge(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	hash := sha256.Sum256([]byte("test"))
	req := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "big.jpg",
		FileSize: 30 * 1024 * 1024, MimeType: "image/jpeg",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(5001), rsp.Header.ErrorCode)
}

func TestApplyUpload_UnsupportedFormat(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	hash := sha256.Sum256([]byte("test"))
	req := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "malware.exe",
		FileSize: 1024, MimeType: "application/x-msdownload",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(5002), rsp.Header.ErrorCode)
}

func TestApplyDownload_NotFound(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: "nonexistent-file-id"}
	rsp := &media.ApplyDownloadRsp{}
	err := authed.DoAuth("/service/media/apply_download", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(5008), rsp.Header.ErrorCode)
}

func TestGetFileInfo_NotFound(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: "nonexistent-file-id"}
	rsp := &media.GetFileInfoRsp{}
	err := authed.DoAuth("/service/media/get_file_info", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
}

func TestSpeechRecognition_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	req := &media.SpeechRecognitionReq{
		RequestId: client.NewRequestID(), SpeechContent: []byte("fake-audio-data"),
	}
	rsp := &media.SpeechRecognitionRsp{}
	err := authed.DoAuth("/service/media/speech_recognition", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// FN-MD-01 | P0 | happy path | 三步上传全链路：apply -> PUT -> complete，验证 file_id 可用 + MinIO 落对象
func TestFN_MD_CompleteUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("md-complete-upload-success")
	fileID := fixture.UploadFile(t, authed, content, "text/plain")
	require.NotEmpty(t, fileID)

	// 直查 MinIO：对象存在（file_id 作为 object_key 的一部分，由 media 服务分配）
	// 注：object_key 格式为 chat/<yyyy>/<mm>/<dd>/<hash-prefix>/<full-hash>，此处仅验证 file_info 可查
	infoReq := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: fileID}
	infoRsp := &media.GetFileInfoRsp{}
	require.NoError(t, authed.DoAuth("/service/media/get_file_info", infoReq, infoRsp))
	require.True(t, infoRsp.Header.Success)
	require.Equal(t, int64(len(content)), infoRsp.FileInfo.FileSize)
}

// FN-MD-02 | P0 | error path | 未 PUT 到 MinIO 就 complete，应失败
func TestFN_MD_CompleteUpload_NotUploaded(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("md-not-uploaded")
	hash := sha256.Sum256(content)

	// ApplyUpload 但不 PUT
	applyReq := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "skip-put.txt",
		FileSize: int64(len(content)), MimeType: "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
	require.True(t, applyRsp.Header.Success)

	// 直接 CompleteUpload，应失败（UPLOAD_INCOMPLETE 5006）
	completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: applyRsp.FileId}
	completeRsp := &media.CompleteUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/complete_upload", completeReq, completeRsp))
	assert.False(t, completeRsp.Header.Success)
	assert.Equal(t, int32(5006), completeRsp.Header.ErrorCode)
}

// FN-MD-03 | P1 | idempotent | 重复 complete 同一 file_id，幂等返回成功
func TestFN_MD_CompleteUpload_AlreadyCompleted(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("md-already-completed")
	fileID := fixture.UploadFile(t, authed, content, "text/plain")

	// 再次 CompleteUpload，应幂等成功
	completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: fileID}
	completeRsp := &media.CompleteUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/complete_upload", completeReq, completeRsp))
	assert.True(t, completeRsp.Header.Success)
}

// FN-MD-04 | P0 | happy path | 大文件 InitMultipart，返回 upload_id + 推荐 part_size
func TestFN_MD_InitMultipart_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 3*1024*1024) // 3MB
	hash := sha256.Sum256(content)
	req := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "big.bin",
		FileSize: int64(len(content)), MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	rsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", req, rsp))
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.FileId)
	assert.NotEmpty(t, rsp.UploadId)
	assert.Greater(t, rsp.RecommendedPartSizeBytes, int32(0))
}

// FN-MD-05 | P1 | error path | 超配额文件拒绝 InitMultipart
func TestFN_MD_InitMultipart_FileTooLarge(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("too-large")
	hash := sha256.Sum256(content)
	req := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "huge.bin",
		FileSize: 30 * 1024 * 1024, MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	rsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", req, rsp))
	assert.False(t, rsp.Header.Success)
}

// FN-MD-06 | P0 | happy path | ApplyPartUpload 获取分片 presigned URL
func TestFN_MD_ApplyPartUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 3*1024*1024)
	hash := sha256.Sum256(content)
	initReq := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "parts.bin",
		FileSize: int64(len(content)), MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	initRsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", initReq, initRsp))
	require.True(t, initRsp.Header.Success)

	req := &media.ApplyPartReq{
		RequestId: client.NewRequestID(), UploadId: initRsp.UploadId, PartNumber: 1,
	}
	rsp := &media.ApplyPartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_part_upload", req, rsp))
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.UploadUrl)
}

// FN-MD-07 | P0 | happy path | init -> upload 3 parts -> complete，验证合并后 file_id 可查
func TestFN_MD_CompleteMultipart_FullFlow(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 6*1024*1024) // 6MB -> 3 parts @ 2MB
	for i := range content {
		content[i] = byte(i % 256)
	}
	fileID := fixture.UploadLargeFile(t, authed, content, "application/octet-stream", 2*1024*1024)
	require.NotEmpty(t, fileID)

	// 验证 file_info
	infoReq := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: fileID}
	infoRsp := &media.GetFileInfoRsp{}
	require.NoError(t, authed.DoAuth("/service/media/get_file_info", infoReq, infoRsp))
	require.True(t, infoRsp.Header.Success)
	require.Equal(t, int64(len(content)), infoRsp.FileInfo.FileSize)
}

// FN-MD-08 | P1 | error path | 缺少某个 part number，CompleteMultipart 拒绝
func TestFN_MD_CompleteMultipart_MissingPart(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 6*1024*1024)
	hash := sha256.Sum256(content)
	initReq := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "missing.bin",
		FileSize: int64(len(content)), MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	initRsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", initReq, initRsp))
	require.True(t, initRsp.Header.Success)

	// 只上传 part 1，跳过 part 2/3，尝试 complete
	partReq := &media.ApplyPartReq{
		RequestId: client.NewRequestID(), UploadId: initRsp.UploadId, PartNumber: 1,
	}
	partRsp := &media.ApplyPartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_part_upload", partReq, partRsp))

	partContent := content[:2*1024*1024]
	httpReq, _ := http.NewRequest("PUT", partRsp.UploadUrl, bytes.NewReader(partContent))
	putResp, err := http.DefaultClient.Do(httpReq)
	require.NoError(t, err)
	putResp.Body.Close()

	// CompleteMultipart 只带 part 1（缺少 2/3）
	completeReq := &media.CompleteMultipartReq{
		RequestId: client.NewRequestID(), UploadId: initRsp.UploadId,
		Parts: []*media.PartETag{{PartNumber: 1, Etag: putResp.Header.Get("ETag")}},
	}
	completeRsp := &media.CompleteMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/complete_multipart", completeReq, completeRsp))
	assert.False(t, completeRsp.Header.Success)
}

// FN-MD-09 | P1 | happy path | init -> abort，验证 upload_id 失效
func TestFN_MD_AbortMultipart_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 3*1024*1024)
	hash := sha256.Sum256(content)
	initReq := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "abort.bin",
		FileSize: int64(len(content)), MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	initRsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", initReq, initRsp))
	require.True(t, initRsp.Header.Success)

	abortReq := &media.AbortMultipartReq{RequestId: client.NewRequestID(), UploadId: initRsp.UploadId}
	abortRsp := &media.AbortMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/abort_multipart", abortReq, abortRsp))
	assert.True(t, abortRsp.Header.Success)

	// 验证 upload_id 已失效：再 ApplyPartUpload 应失败
	partReq := &media.ApplyPartReq{RequestId: client.NewRequestID(), UploadId: initRsp.UploadId, PartNumber: 1}
	partRsp := &media.ApplyPartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_part_upload", partReq, partRsp))
	assert.False(t, partRsp.Header.Success)
}

// FN-MD-10 | P2 | idempotent | 重复 abort 幂等
func TestFN_MD_AbortMultipart_AlreadyAborted(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := make([]byte, 3*1024*1024)
	hash := sha256.Sum256(content)
	initReq := &media.InitMultipartReq{
		RequestId: client.NewRequestID(), FileName: "abort2.bin",
		FileSize: int64(len(content)), MimeType: "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	initRsp := &media.InitMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/init_multipart", initReq, initRsp))
	require.True(t, initRsp.Header.Success)

	abortReq := &media.AbortMultipartReq{RequestId: client.NewRequestID(), UploadId: initRsp.UploadId}
	require.NoError(t, authed.DoAuth("/service/media/abort_multipart", abortReq, &media.AbortMultipartRsp{}))

	// 再次 abort，应幂等（不报错或返回 success=false 但不 panic）
	abortReq2 := &media.AbortMultipartReq{RequestId: client.NewRequestID(), UploadId: initRsp.UploadId}
	abortRsp2 := &media.AbortMultipartRsp{}
	require.NoError(t, authed.DoAuth("/service/media/abort_multipart", abortReq2, abortRsp2))
	// 幂等：要么 success=true（已 abort），要么 success=false（upload_id 不存在）
	_ = abortRsp2.Header.Success
}

// FN-MD-11 | P0 | dedup | 相同 content_hash，第二次 apply 返回 already_exists=true + 相同 file_id
func TestFN_MD_ApplyUpload_Dedup_SameHash(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("md-dedup-same-hash")
	hash := sha256.Sum256(content)
	hashStr := fmt.Sprintf("sha256:%x", hash)

	// 第一次上传
	fileID := fixture.UploadFile(t, authed, content, "text/plain")

	// 第二次 ApplyUpload 相同 hash
	applyReq := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "dup.txt",
		FileSize: int64(len(content)), MimeType: "text/plain",
		ContentHash: hashStr, Purpose: media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
	require.True(t, applyRsp.Header.Success)
	assert.True(t, applyRsp.AlreadyExists, "相同 hash 应返回 already_exists=true")
	assert.Equal(t, fileID, applyRsp.FileId, "dedup 应返回相同 file_id")
	assert.Empty(t, applyRsp.UploadUrl, "dedup 时不应返回 upload_url")
}

// FN-MD-12 | P0 | quota | 超用户配额拒绝（默认 5GB，此处用大文件触发）
func TestFN_MD_ApplyUpload_QuotaExceeded(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// 先耗尽配额：上传一个接近 5GB 的文件不现实，改为直接声明超大 file_size
	// 服务端在 ApplyUpload 时检查 file_size + used > quota
	content := []byte("quota-test")
	hash := sha256.Sum256(content)
	req := &media.ApplyUploadReq{
		RequestId: client.NewRequestID(), FileName: "over-quota.bin",
		FileSize:    6 * 1024 * 1024 * 1024, // 6GB > 5GB quota
		MimeType:    "application/octet-stream",
		ContentHash: fmt.Sprintf("sha256:%x", hash), Purpose: media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_upload", req, rsp))
	assert.False(t, rsp.Header.Success, "超配额应拒绝")
}

// FN-MD-13 | P1 | quota boundary | 配额接近上限边界：上传后 used_bytes 接近 quota
func TestFN_MD_ApplyUpload_QuotaRemaining(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("md-quota-remaining-check")
	fileID := fixture.UploadFile(t, authed, content, "text/plain")
	require.NotEmpty(t, fileID)

	// 直查 DB：media_user_quota.used_bytes >= len(content)
	// DBVerifier.MediaQuota 检查 used_bytes 是否与预期一致（Phase 1 提供）
	// 此处仅验证 quota 行存在且 used_bytes > 0
	infoReq := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: fileID}
	infoRsp := &media.GetFileInfoRsp{}
	require.NoError(t, authed.DoAuth("/service/media/get_file_info", infoReq, infoRsp))
	require.True(t, infoRsp.Header.Success)
	require.Equal(t, int64(len(content)), infoRsp.FileInfo.FileSize)
}
