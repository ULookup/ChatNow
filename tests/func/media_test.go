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
		RequestId: client.NewRequestID(), FileName: "big.zip",
		FileSize: 200 * 1024 * 1024, MimeType: "application/zip",
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
