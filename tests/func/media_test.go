//go:build func

package func_test

import (
	"crypto/sha256"
	"fmt"
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
