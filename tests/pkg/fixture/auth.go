package fixture

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math/rand"
	"strings"
	"testing"

	"chatnow-tests/pkg/client"
	identity "chatnow-tests/proto/chatnow/identity"
)

// RegisterAndLogin creates a new user with random credentials and returns the authed client.
func RegisterAndLogin(t testing.TB, c *client.HTTPClient) (*client.HTTPClient, string, string) {
	username := fmt.Sprintf("test_%d_%d", rand.Int63n(1000000), rand.Intn(1000))
	password := "test123456"
	nickname := username

	req := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: nickname,
	}
	rsp := &identity.RegisterRsp{}
	if err := c.DoNoAuth("/service/identity/register", req, rsp); err != nil {
		t.Fatalf("Register: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("Register failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}

	authed := client.NewHTTPClient(c.Config())
	authed.AccessToken = rsp.Tokens.AccessToken
	authed.RefreshToken = rsp.Tokens.RefreshToken
	authed.UserID = rsp.UserId
	// Registration has no device input. ACKs must use the device Identity issued.
	parts := strings.Split(authed.AccessToken, ".")
	if len(parts) != 3 {
		t.Fatal("registration returned malformed access token")
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		t.Fatal("registration returned malformed token payload")
	}
	var claims struct {
		DeviceID string `json:"did"`
	}
	if json.Unmarshal(payload, &claims) != nil || claims.DeviceID == "" {
		t.Fatal("registration token has no device identity")
	}
	authed.DeviceID = claims.DeviceID
	return authed, username, password
}

// LoginUser logs in an existing user and returns the authed client.
func LoginUser(t testing.TB, c *client.HTTPClient, username, password string) *client.HTTPClient {
	req := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		DeviceId:   client.NewDeviceID(),
		DeviceName: "test-device",
	}
	rsp := &identity.LoginRsp{}
	if err := c.DoNoAuth("/service/identity/login", req, rsp); err != nil {
		t.Fatalf("Login: %v", err)
	}
	if !rsp.Header.Success {
		t.Fatalf("Login failed: code=%d msg=%s", rsp.Header.ErrorCode, rsp.Header.ErrorMessage)
	}

	authed := client.NewHTTPClient(c.Config())
	authed.AccessToken = rsp.Tokens.AccessToken
	authed.RefreshToken = rsp.Tokens.RefreshToken
	authed.UserID = rsp.UserInfo.UserId
	authed.DeviceID = req.DeviceId
	return authed
}
