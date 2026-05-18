package client

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"time"

	"google.golang.org/protobuf/proto"
)

type HTTPClient struct {
	cfg     *Config
	baseURL string
	client  *http.Client

	// Per-test-session JWT
	AccessToken  string
	RefreshToken string
	UserID       string
	DeviceID     string
}

func NewHTTPClient(cfg *Config) *HTTPClient {
	return &HTTPClient{
		cfg:     cfg,
		baseURL: "http://" + cfg.Target.GatewayAddr,
		client:  &http.Client{Timeout: time.Duration(cfg.Timeout.HTTPRequestSec) * time.Second},
	}
}

// NewRequestID generates a 32-hex-char trace/request ID.
func NewRequestID() string {
	b := make([]byte, 16)
	rand.Reader.Read(b)
	return hex.EncodeToString(b)
}

// NewDeviceID generates a random device ID.
func NewDeviceID() string {
	b := make([]byte, 8)
	rand.Reader.Read(b)
	return hex.EncodeToString(b)
}

// Do sends a protobuf request to path and unmarshals the protobuf response into resp.
func (c *HTTPClient) Do(path string, req proto.Message, resp proto.Message, accessToken string) error {
	body, err := proto.Marshal(req)
	if err != nil {
		return fmt.Errorf("marshal request: %w", err)
	}

	httpReq, err := http.NewRequest("POST", c.baseURL+path, bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("create request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/x-protobuf")
	if accessToken != "" {
		httpReq.Header.Set("Authorization", "Bearer "+accessToken)
	}

	httpResp, err := c.client.Do(httpReq)
	if err != nil {
		return fmt.Errorf("http request: %w", err)
	}
	defer httpResp.Body.Close()

	respBody, err := io.ReadAll(httpResp.Body)
	if err != nil {
		return fmt.Errorf("read response: %w", err)
	}

	if httpResp.StatusCode != 200 {
		return fmt.Errorf("http status %d: %s", httpResp.StatusCode, string(respBody))
	}

	if err := proto.Unmarshal(respBody, resp); err != nil {
		return fmt.Errorf("unmarshal response: %w", err)
	}
	return nil
}

// DoNoAuth sends without Authorization header (for whitelisted endpoints).
func (c *HTTPClient) DoNoAuth(path string, req proto.Message, resp proto.Message) error {
	return c.Do(path, req, resp, "")
}

// DoAuth sends with the client's stored AccessToken.
func (c *HTTPClient) DoAuth(path string, req proto.Message, resp proto.Message) error {
	return c.Do(path, req, resp, c.AccessToken)
}

// Config returns the client's Config.
func (c *HTTPClient) Config() *Config { return c.cfg }
