package verify

import (
	"bytes"
	"context"
	"os"
	"testing"

	"github.com/minio/minio-go/v7"
	"github.com/stretchr/testify/require"
)

func TestMinIOVerifier_ObjectExists(t *testing.T) {
	endpoint := os.Getenv("MINIO_ENDPOINT")
	if endpoint == "" {
		t.Skip("MINIO_ENDPOINT not set")
	}
	v := NewMinIOVerifier(endpoint,
		os.Getenv("MINIO_ACCESS_KEY"), os.Getenv("MINIO_SECRET_KEY"))
	ctx := context.Background()

	bucket := "chatnow-media-private"
	key := "test/verify-minio-exists"
	content := []byte("hello-minio-verify")

	_ = v.client.RemoveObject(ctx, bucket, key, minio.RemoveObjectOptions{}) // cleanup if exists
	_, err := v.client.PutObject(ctx, bucket, key, bytes.NewReader(content), int64(len(content)), minio.PutObjectOptions{})
	require.NoError(t, err)

	v.ObjectExists(t, bucket, key)
}

func TestMinIOVerifier_ObjectContent(t *testing.T) {
	endpoint := os.Getenv("MINIO_ENDPOINT")
	if endpoint == "" {
		t.Skip("MINIO_ENDPOINT not set")
	}
	v := NewMinIOVerifier(endpoint,
		os.Getenv("MINIO_ACCESS_KEY"), os.Getenv("MINIO_SECRET_KEY"))
	ctx := context.Background()

	bucket := "chatnow-media-private"
	key := "test/verify-minio-content"
	content := []byte("content-check-42")

	_ = v.client.RemoveObject(ctx, bucket, key, minio.RemoveObjectOptions{})
	_, err := v.client.PutObject(ctx, bucket, key, bytes.NewReader(content), int64(len(content)), minio.PutObjectOptions{})
	require.NoError(t, err)

	v.ObjectContent(t, bucket, key, content)
}

func TestMinIOVerifier_ObjectCount(t *testing.T) {
	endpoint := os.Getenv("MINIO_ENDPOINT")
	if endpoint == "" {
		t.Skip("MINIO_ENDPOINT not set")
	}
	v := NewMinIOVerifier(endpoint,
		os.Getenv("MINIO_ACCESS_KEY"), os.Getenv("MINIO_SECRET_KEY"))

	// 对象数应 >= 0（bucket 存在即可）
	v.ObjectCount(t, "chatnow-media-private", 0)
}
