//go:build bvt

package bvt_test

import (
	"fmt"
	"math/rand"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// BVT-004 | P0 | 认证链路 | 用户名注册成功，返回 user_id
func TestBVT_Register_Success(t *testing.T) {
	username := fmt.Sprintf("bvt_%d_%d", rand.Int63n(10000000), rand.Intn(1000))
	password := "Bvt123456"

	req := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	rsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "注册失败: %s", rsp.Header.ErrorMessage)
	assert.NotEmpty(t, rsp.UserId)
	require.NotNil(t, rsp.Tokens)
	assert.NotEmpty(t, rsp.Tokens.AccessToken)
}

// BVT-005 | P0 | 认证链路 | 登录成功，返回 access_token + refresh_token
func TestBVT_Login_Success(t *testing.T) {
	// 先注册
	username := fmt.Sprintf("bvt_%d_%d", rand.Int63n(10000000), rand.Intn(1000))
	password := "Bvt123456"

	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	require.NoError(t, HTTP.DoNoAuth("/service/identity/register", regReq, regRsp))
	require.True(t, regRsp.Header.Success)

	// 再登录
	loginReq := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{Username: username, Password: password},
		},
		DeviceId:   client.NewDeviceID(),
		DeviceName: "bvt-test-device",
	}
	loginRsp := &identity.LoginRsp{}
	err := HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp)
	require.NoError(t, err)
	require.True(t, loginRsp.Header.Success, "登录失败: %s", loginRsp.Header.ErrorMessage)
	require.NotNil(t, loginRsp.Tokens)
	assert.NotEmpty(t, loginRsp.Tokens.AccessToken)
	assert.NotEmpty(t, loginRsp.Tokens.RefreshToken)
}

// BVT-006 | P0 | 认证链路 | 带 token 调 GetProfile，返回自身信息
func TestBVT_AuthenticatedAPICall(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &identity.GetProfileReq{RequestId: client.NewRequestID()}
	rsp := &identity.GetProfileRsp{}
	err := authed.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	require.True(t, rsp.Header.Success, "鉴权调用失败: %s", rsp.Header.ErrorMessage)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, authed.UserID, rsp.UserInfo.UserId)
}
