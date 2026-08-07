package verify

import (
	"context"
	"io"
	"testing"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"github.com/stretchr/testify/require"
)

// MinIOVerifier 直查 MinIO 对象存储，验证媒体文件落库一致性。
type MinIOVerifier struct {
	client *minio.Client
}

// NewMinIOVerifier 创建 MinIO 验证器。
// Credentials must be supplied by the isolated test environment.
func NewMinIOVerifier(endpoint, accessKey, secretKey string) *MinIOVerifier {
	if endpoint == "" {
		panic("NewMinIOVerifier: endpoint is required")
	}
	if accessKey == "" {
		panic("NewMinIOVerifier: access key is required")
	}
	if secretKey == "" {
		panic("NewMinIOVerifier: secret key is required")
	}
	cli, err := minio.New(endpoint, &minio.Options{
		Creds:        credentials.NewStaticV4(accessKey, secretKey, ""),
		Secure:       false,
		BucketLookup: minio.BucketLookupPath,
	})
	if err != nil {
		panic("NewMinIOVerifier: " + err.Error())
	}
	return &MinIOVerifier{client: cli}
}

// ObjectExists 验证指定 bucket/key 的对象存在（HEAD 检查）。
func (v *MinIOVerifier) ObjectExists(t testing.TB, bucket, key string) {
	t.Helper()
	_, err := v.client.StatObject(context.Background(), bucket, key, minio.StatObjectOptions{})
	require.NoError(t, err, "MinIO 对象 %s/%s 不存在", bucket, key)
}

// ObjectContent 验证指定 bucket/key 的对象内容与 expected 一致。
func (v *MinIOVerifier) ObjectContent(t testing.TB, bucket, key string, expected []byte) {
	t.Helper()
	obj, err := v.client.GetObject(context.Background(), bucket, key, minio.GetObjectOptions{})
	require.NoError(t, err, "获取 MinIO 对象 %s/%s 失败", bucket, key)
	defer obj.Close()
	body, err := io.ReadAll(obj)
	require.NoError(t, err, "读取 MinIO 对象 %s/%s 内容失败", bucket, key)
	require.Equal(t, expected, body, "MinIO 对象 %s/%s 内容不符", bucket, key)
}

// ObjectCount 验证指定 bucket 中的对象数 >= expected（用 ListObjects 计数）。
// 传 0 表示仅验证 bucket 可列举（对象数 >= 0）。
func (v *MinIOVerifier) ObjectCount(t testing.TB, bucket string, expected int) {
	t.Helper()
	ctx := context.Background()
	cnt := 0
	for obj := range v.client.ListObjects(ctx, bucket, minio.ListObjectsOptions{Recursive: true}) {
		if obj.Err != nil {
			require.NoError(t, obj.Err, "列举 MinIO bucket %s 失败", bucket)
		}
		cnt++
	}
	if expected > 0 {
		require.GreaterOrEqual(t, cnt, expected, "MinIO bucket %s 对象数 %d 应 >= %d", bucket, cnt, expected)
	}
}
