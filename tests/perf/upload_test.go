//go:build perf

package perf_test

import (
	"crypto/sha256"
	"fmt"
	"testing"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	media "chatnow-tests/proto/chatnow/media"
)

func BenchmarkApplyUpload(b *testing.B) {
	authed, _, _ := fixture.RegisterAndLogin(b, HTTP)
	hash := sha256.Sum256([]byte("benchmark content"))

	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			req := &media.ApplyUploadReq{
				RequestId: client.NewRequestID(), FileName: "bench.png",
				FileSize: 1024, MimeType: "image/png",
				ContentHash: fmt.Sprintf("sha256:%x", hash),
				Purpose:     media.MediaPurpose_CHAT,
			}
			rsp := &media.ApplyUploadRsp{}
			if err := authed.DoAuth("/service/media/apply_upload", req, rsp); err != nil {
				b.Fatal(err)
			}
			if !rsp.Header.Success {
				b.Fatal("upload failed")
			}
		}
	})
}
