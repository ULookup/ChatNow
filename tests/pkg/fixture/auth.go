package fixture

import (
	"fmt"
	"math/rand"
	"testing"

	"chatnow-tests/pkg/client"
	identity "chatnow-tests/proto/chatnow/identity"
)

// RegisterAndLogin creates a new user with random credentials and returns the authed client.
func RegisterAndLogin(t testing.TB, c *client.HTTPClient) (*client.HTTPClient, string, string) {
	username := fmt.Sprintf("test_%d_%d", rand.Int63(), rand.Intn(10000))
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
	authed.DeviceID = client.NewDeviceID()
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
	authed.DeviceID = client.NewDeviceID()
	return authed
}
