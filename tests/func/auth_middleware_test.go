//go:build func

package func_test

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// Whitelisted endpoints should work without JWT

func TestWhitelist_Register_NoAuth(t *testing.T) {
	req := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: randUser(), Password: "test123456"},
		},
		Nickname: randUser(),
	}
	rsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestWhitelist_Login_NoAuth(t *testing.T) {
	authed, username, password := fixture.RegisterAndLogin(t, HTTP)
	_ = authed
	req := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		DeviceId: client.NewDeviceID(), DeviceName: "test",
	}
	rsp := &identity.LoginRsp{}
	err := HTTP.DoNoAuth("/service/identity/login", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

func TestWhitelist_SendVerifyCode_NoAuth(t *testing.T) {
	req := &identity.SendVerifyCodeReq{
		RequestId: client.NewRequestID(),
		Destination: &identity.SendVerifyCodeReq_Email{Email: "test@example.com"},
	}
	rsp := &identity.SendVerifyCodeRsp{}
	err := HTTP.DoNoAuth("/service/identity/send_verify_code", req, rsp)
	require.NoError(t, err)
	assert.True(t, rsp.Header.Success)
}

// JWT-required endpoints should fail without token

func TestJWTRequired_GetProfile_NoToken(t *testing.T) {
	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := HTTP.DoNoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
}

func TestJWTRequired_ExpiredToken(t *testing.T) {
	expiredClient := client.NewHTTPClient(HTTP.Config())
	expiredClient.AccessToken = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U"
	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := expiredClient.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	assert.False(t, rsp.Header.Success)
}
