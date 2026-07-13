//go:build func

package fixture

import (
	"crypto/sha256"
	"fmt"
	"testing"

	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	media "chatnow-tests/proto/chatnow/media"
)

func TestUploadFile_FullFlow(t *testing.T) {
	authed, _, _ := RegisterAndLogin(t, HTTP)
	content := []byte("fixture-upload-test")
	fileID := UploadFile(t, authed, content, "text/plain")
	require.NotEmpty(t, fileID)

	// 验证 file_id 可查询
	req := &media.GetFileInfoReq{RequestId: client.NewRequestID(), FileId: fileID}
	rsp := &media.GetFileInfoRsp{}
	require.NoError(t, authed.DoAuth("/service/media/get_file_info", req, rsp))
	require.True(t, rsp.Header.Success)
	require.Equal(t, int64(len(content)), rsp.FileInfo.FileSize)
}

func TestUploadLargeFile_Multipart(t *testing.T) {
	authed, _, _ := RegisterAndLogin(t, HTTP)
	content := make([]byte, 6*1024*1024) // 6MB -> 3 parts @ 2MB
	for i := range content {
		content[i] = byte(i % 256)
	}
	hash := sha256.Sum256(content)
	_ = fmt.Sprintf("sha256:%x", hash)

	fileID := UploadLargeFile(t, authed, content, "application/octet-stream", 2*1024*1024)
	require.NotEmpty(t, fileID)
}
