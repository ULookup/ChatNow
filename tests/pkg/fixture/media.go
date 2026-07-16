package fixture

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"net/http"
	"testing"

	"chatnow-tests/pkg/client"
	media "chatnow-tests/proto/chatnow/media"
)

// UploadFile 完成三步上传（ApplyUpload -> PUT MinIO -> CompleteUpload）并返回 file_id。
// 适用于单段上传（<=100MB）。content 为文件内容，mime 为 MIME 类型。
func UploadFile(t testing.TB, c *client.HTTPClient, content []byte, mime string) string {
	return UploadFileForPurpose(t, c, content, mime, media.MediaPurpose_CHAT)
}

// UploadFileForPurpose 完成指定用途的三步上传并返回 file_id。
func UploadFileForPurpose(t testing.TB, c *client.HTTPClient, content []byte, mime string, purpose media.MediaPurpose) string {
	t.Helper()
	hash := sha256.Sum256(content)
	req := &media.ApplyUploadReq{
		RequestId:   client.NewRequestID(),
		FileName:    "fixture.bin",
		FileSize:    int64(len(content)),
		MimeType:    mime,
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     purpose,
	}
	rsp := &media.ApplyUploadRsp{}
	if err := c.DoAuth("/service/media/apply_upload", req, rsp); err != nil {
		t.Fatalf("ApplyUpload: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("ApplyUpload failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}
	if rsp.AlreadyExists {
		return rsp.FileId // dedup 命中
	}

	// PUT 到 presigned URL
	httpReq, err := http.NewRequest("PUT", rsp.UploadUrl, bytes.NewReader(content))
	if err != nil {
		t.Fatalf("create PUT request: %v", err)
	}
	if rsp.Headers != nil {
		for k, v := range rsp.Headers {
			httpReq.Header.Set(k, v)
		}
	}
	putResp, err := http.DefaultClient.Do(httpReq)
	if err != nil {
		t.Fatalf("PUT to MinIO: %v", err)
	}
	defer putResp.Body.Close()
	if putResp.StatusCode != 200 {
		t.Fatalf("PUT MinIO status %d", putResp.StatusCode)
	}

	// CompleteUpload
	completeReq := &media.CompleteUploadReq{RequestId: client.NewRequestID(), FileId: rsp.FileId}
	completeRsp := &media.CompleteUploadRsp{}
	if err := c.DoAuth("/service/media/complete_upload", completeReq, completeRsp); err != nil {
		t.Fatalf("CompleteUpload: %v", err)
	}
	if !completeRsp.Header.Success {
		t.Fatalf("CompleteUpload failed: code=%d msg=%s", completeRsp.Header.ErrorCode, completeRsp.Header.ErrorMessage)
	}
	return rsp.FileId
}

// UploadLargeFile 完成分片上传（InitMultipart -> ApplyPartUpload * N -> PUT -> CompleteMultipart）并返回 file_id。
// content 为完整文件内容，partSize 为每片大小（字节）。
func UploadLargeFile(t testing.TB, c *client.HTTPClient, content []byte, mime string, partSize int) string {
	t.Helper()
	hash := sha256.Sum256(content)
	initReq := &media.InitMultipartReq{
		RequestId:   client.NewRequestID(),
		FileName:    "fixture-large.bin",
		FileSize:    int64(len(content)),
		MimeType:    mime,
		ContentHash: fmt.Sprintf("sha256:%x", hash),
		Purpose:     media.MediaPurpose_CHAT,
	}
	initRsp := &media.InitMultipartRsp{}
	if err := c.DoAuth("/service/media/init_multipart", initReq, initRsp); err != nil {
		t.Fatalf("InitMultipart: %v", err)
	}
	if !initRsp.Header.Success {
		t.Fatalf("InitMultipart failed: code=%d msg=%s", initRsp.Header.ErrorCode, initRsp.Header.ErrorMessage)
	}
	if initRsp.RecommendedPartSizeBytes > 0 {
		partSize = int(initRsp.RecommendedPartSizeBytes)
	}

	uploadID := initRsp.UploadId
	parts := make([]*media.PartETag, 0)
	offset := 0
	partNum := int32(1)
	for offset < len(content) {
		end := offset + partSize
		if end > len(content) {
			end = len(content)
		}
		partContent := content[offset:end]

		applyReq := &media.ApplyPartReq{
			RequestId:  client.NewRequestID(),
			UploadId:   uploadID,
			PartNumber: partNum,
		}
		applyRsp := &media.ApplyPartRsp{}
		if err := c.DoAuth("/service/media/apply_part_upload", applyReq, applyRsp); err != nil {
			t.Fatalf("ApplyPartUpload #%d: %v", partNum, err)
		}
		if !applyRsp.Header.Success {
			t.Fatalf("ApplyPartUpload #%d failed: code=%d", partNum, applyRsp.Header.ErrorCode)
		}

		httpReq, err := http.NewRequest("PUT", applyRsp.UploadUrl, bytes.NewReader(partContent))
		if err != nil {
			t.Fatalf("create PUT part request: %v", err)
		}
		putResp, err := http.DefaultClient.Do(httpReq)
		if err != nil {
			t.Fatalf("PUT part #%d: %v", partNum, err)
		}
		putResp.Body.Close()
		if putResp.StatusCode != 200 {
			t.Fatalf("PUT part #%d status %d", partNum, putResp.StatusCode)
		}
		parts = append(parts, &media.PartETag{
			PartNumber: partNum,
			Etag:       putResp.Header.Get("ETag"),
		})

		offset = end
		partNum++
	}

	completeReq := &media.CompleteMultipartReq{
		RequestId: client.NewRequestID(),
		UploadId:  uploadID,
		Parts:     parts,
	}
	completeRsp := &media.CompleteMultipartRsp{}
	if err := c.DoAuth("/service/media/complete_multipart", completeReq, completeRsp); err != nil {
		t.Fatalf("CompleteMultipart: %v", err)
	}
	if !completeRsp.Header.Success {
		t.Fatalf("CompleteMultipart failed: code=%d msg=%s", completeRsp.Header.ErrorCode, completeRsp.Header.ErrorMessage)
	}
	return initRsp.FileId
}
