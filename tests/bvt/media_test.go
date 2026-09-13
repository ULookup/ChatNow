//go:build bvt

package bvt_test

import (
	"bytes"
	"crypto/sha256"
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	media "chatnow-tests/proto/chatnow/media"
)

// BVT-015 | P0 | 媒体链路 | 申请上传，返回 file_id + upload_url
func TestBVT_ApplyUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt test content")
	hash := sha256.Sum256(content)

	req := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	rsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "apply_upload 失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.FileId)
	// upload_url 可能为空（如果 already_exists=true）
	if !rsp.AlreadyExists {
		assert.NotEmpty(t, rsp.UploadUrl)
	}
}

// BVT-016 | P0 | 媒体链路 | PUT 到 MinIO + CompleteUpload，success=true
func TestBVT_CompleteUpload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt upload content")
	hash := sha256.Sum256(content)

	// Step 1: ApplyUpload
	applyReq := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt-upload.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	err := authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp)
	require.NoError(t, err)
	require.True(t, applyRsp.Header.Success)

	if applyRsp.AlreadyExists {
		t.Skip("文件已存在（dedup 命中），跳过上传步骤")
	}

	fileID := applyRsp.FileId
	uploadURL := applyRsp.UploadUrl
	require.NotEmpty(t, uploadURL, "upload_url 不应为空")

	// Step 2: PUT 到 MinIO presigned URL
	httpReq, _ := http.NewRequest("PUT", uploadURL, bytes.NewReader(content))
	for k, v := range applyRsp.Headers {
		httpReq.Header.Set(k, v)
	}
	putResp, err := http.DefaultClient.Do(httpReq)
	require.NoError(t, err)
	if putResp.StatusCode != http.StatusOK {
		var detail struct {
			Code    string `xml:"Code"`
			Message string `xml:"Message"`
		}
		_ = xml.NewDecoder(io.LimitReader(putResp.Body, 4096)).Decode(&detail)
		putResp.Body.Close()
		t.Fatalf("S3 PUT failed: status=%d code=%s message=%s", putResp.StatusCode, detail.Code, detail.Message)
	}
	putResp.Body.Close()

	// Step 3: CompleteUpload
	completeReq := &media.CompleteUploadReq{
		RequestId: client.NewRequestID(),
		FileId:    fileID,
	}
	completeRsp := &media.CompleteUploadRsp{}
	err = authed.DoAuth("/service/media/complete_upload", completeReq, completeRsp)
	require.NoError(t, err)
	require.True(t, completeRsp.Header.Success, "complete_upload 失败: %s", completeRsp.Header.ErrorMessage)
}

// BVT-017 | P0 | 媒体链路 | 申请下载，返回 download_url，内容匹配
func TestBVT_ApplyDownload_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	content := []byte("bvt download content")
	hash := sha256.Sum256(content)

	// 先完成上传
	applyReq := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "bvt-dl.txt",
		FileSize:    int64(len(content)),
		MimeType:    "text/plain",
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	applyRsp := &media.ApplyUploadRsp{}
	require.NoError(t, authed.DoAuth("/service/media/apply_upload", applyReq, applyRsp))
	require.True(t, applyRsp.Header.Success)
	fileID := applyRsp.FileId

	if !applyRsp.AlreadyExists {
		httpReq, _ := http.NewRequest("PUT", applyRsp.UploadUrl, bytes.NewReader(content))
		for k, v := range applyRsp.Headers {
			httpReq.Header.Set(k, v)
		}
		putResp, err := http.DefaultClient.Do(httpReq)
		require.NoError(t, err)
		require.Equal(t, 200, putResp.StatusCode)
		putResp.Body.Close()

		completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: fileID}
		require.NoError(t, authed.DoAuth("/service/media/complete_upload", completeReq, &media.CompleteUploadRsp{}))
	}

	// 申请下载
	dlReq := &media.ApplyDownloadReq{RequestId: client.NewRequestID(), FileId: fileID}
	dlRsp := &media.ApplyDownloadRsp{}
	err := authed.DoAuth("/service/media/apply_download", dlReq, dlRsp)
	require.NoError(t, err)
	require.True(t, dlRsp.Header.Success, "apply_download 失败: %s", dlRsp.Header.ErrorMessage)
	require.NotEmpty(t, dlRsp.DownloadUrl)

	// 下载并验证内容
	dlResp, err := http.Get(dlRsp.DownloadUrl)
	require.NoError(t, err)
	defer dlResp.Body.Close()
	require.Equal(t, 200, dlResp.StatusCode)
	body, _ := io.ReadAll(dlResp.Body)
	assert.Equal(t, content, body, "下载内容与上传不一致")
}
