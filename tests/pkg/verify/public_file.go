package verify

import (
	"bytes"
	"io"
	"net/http"
	"testing"
	"time"
)

// FileURLContentEquals verifies an HTTP GET without additional credentials without logging the URL or bytes.
func FileURLContentEquals(t testing.TB, url string, expected []byte) {
	t.Helper()
	client := &http.Client{Timeout: 10 * time.Second}
	response, err := client.Get(url)
	if err != nil {
		t.Fatal("file URL request failed")
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("file URL returned HTTP %d", response.StatusCode)
	}
	actual, err := io.ReadAll(io.LimitReader(response.Body, int64(len(expected))+1))
	if err != nil || !bytes.Equal(actual, expected) {
		t.Fatal("file URL content differs from uploaded bytes")
	}
}
