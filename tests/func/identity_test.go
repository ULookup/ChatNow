//go:build func

package func_test

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

func randUser() string {
	return fmt.Sprintf("test_%d_%d", rand.Int63(), rand.Intn(10000))
}

// ---------------------------------------------------------------------------
// 1. Register
// ---------------------------------------------------------------------------

func TestRegister_UsernamePassword_Success(t *testing.T) {
	username := randUser()
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
	err := HTTP.DoNoAuth("/service/identity/register", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.UserId)
	require.NotNil(t, rsp.Tokens)
	assert.NotEmpty(t, rsp.Tokens.AccessToken)
	assert.NotEmpty(t, rsp.Tokens.RefreshToken)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, nickname, rsp.UserInfo.Nickname)
}

func TestRegister_DuplicateUsername_Error(t *testing.T) {
	username := randUser()
	password := "test123456"

	// First registration — succeeds.
	req1 := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	rsp1 := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", req1, rsp1)
	require.NoError(t, err)
	require.True(t, rsp1.Header.Success)

	// Second registration with same username — error_code=1005.
	req2 := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: "different_name",
	}
	rsp2 := &identity.RegisterRsp{}
	err = HTTP.DoNoAuth("/service/identity/register", req2, rsp2)
	require.NoError(t, err)
	require.NotNil(t, rsp2.Header)
	assert.False(t, rsp2.Header.Success)
	assert.Equal(t, int32(1005), rsp2.Header.ErrorCode)
}

func TestRegister_InvalidParams_Error(t *testing.T) {
	// Short password.
	req1 := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: randUser(),
				Password: "ab",
			},
		},
		Nickname: "valid_nick",
	}
	rsp1 := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", req1, rsp1)
	require.NoError(t, err)
	require.NotNil(t, rsp1.Header)
	assert.False(t, rsp1.Header.Success)
	assert.Equal(t, int32(9004), rsp1.Header.ErrorCode)

	// Empty nickname.
	req2 := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: randUser(),
				Password: "valid_password_123",
			},
		},
		Nickname: "",
	}
	rsp2 := &identity.RegisterRsp{}
	err = HTTP.DoNoAuth("/service/identity/register", req2, rsp2)
	require.NoError(t, err)
	require.NotNil(t, rsp2.Header)
	assert.False(t, rsp2.Header.Success)
	assert.Equal(t, int32(9004), rsp2.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 2. Login
// ---------------------------------------------------------------------------

func TestLogin_Success(t *testing.T) {
	username := randUser()
	password := "test123456"

	// Register first.
	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", regReq, regRsp)
	require.NoError(t, err)
	require.True(t, regRsp.Header.Success)

	// Re-login with the same credentials.
	loginReq := &identity.LoginReq{
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
	loginRsp := &identity.LoginRsp{}
	err = HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp)
	require.NoError(t, err)
	require.NotNil(t, loginRsp.Header)
	assert.True(t, loginRsp.Header.Success)
	require.NotNil(t, loginRsp.Tokens)
	assert.NotEmpty(t, loginRsp.Tokens.AccessToken)
	assert.NotEmpty(t, loginRsp.Tokens.RefreshToken)
}

func TestLogin_WrongPassword_Error(t *testing.T) {
	username := randUser()
	password := "correct123"

	// Register first.
	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", regReq, regRsp)
	require.NoError(t, err)
	require.True(t, regRsp.Header.Success)

	// Login with wrong password.
	loginReq := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: "wrong_password",
			},
		},
		DeviceId:   client.NewDeviceID(),
		DeviceName: "test-device",
	}
	loginRsp := &identity.LoginRsp{}
	err = HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp)
	require.NoError(t, err)
	require.NotNil(t, loginRsp.Header)
	assert.False(t, loginRsp.Header.Success)
	assert.Equal(t, int32(1001), loginRsp.Header.ErrorCode)
}

func TestLogin_UserNotFound_Error(t *testing.T) {
	loginReq := &identity.LoginReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.LoginReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: randUser(), // nonexistent
				Password: "irrelevant",
			},
		},
		DeviceId:   client.NewDeviceID(),
		DeviceName: "test-device",
	}
	loginRsp := &identity.LoginRsp{}
	err := HTTP.DoNoAuth("/service/identity/login", loginReq, loginRsp)
	require.NoError(t, err)
	require.NotNil(t, loginRsp.Header)
	assert.False(t, loginRsp.Header.Success)
	assert.Equal(t, int32(1004), loginRsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 3. Logout
// ---------------------------------------------------------------------------

func TestLogout_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// Logout.
	logoutReq := &identity.LogoutReq{
		RequestId: client.NewRequestID(),
	}
	logoutRsp := &identity.LogoutRsp{}
	err := authed.DoAuth("/service/identity/logout", logoutReq, logoutRsp)
	require.NoError(t, err)
	require.NotNil(t, logoutRsp.Header)
	assert.True(t, logoutRsp.Header.Success)

	// Verify the token is now invalid by trying to call get_profile.
	profileReq := &identity.GetProfileReq{
		RequestId: client.NewRequestID(),
	}
	profileRsp := &identity.GetProfileRsp{}
	err = authed.DoAuth("/service/identity/get_profile", profileReq, profileRsp)
	require.NoError(t, err)
	require.NotNil(t, profileRsp.Header)
	assert.False(t, profileRsp.Header.Success)
}

func TestLogout_NoToken_Error(t *testing.T) {
	req := &identity.LogoutReq{
		RequestId: client.NewRequestID(),
	}
	rsp := &identity.LogoutRsp{}
	err := HTTP.DoNoAuth("/service/identity/logout", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.False(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 4. SendVerifyCode
// ---------------------------------------------------------------------------

func TestSendVerifyCode_Email_Success(t *testing.T) {
	req := &identity.SendVerifyCodeReq{
		RequestId: client.NewRequestID(),
		Destination: &identity.SendVerifyCodeReq_Email{
			Email: "test@example.com",
		},
	}
	rsp := &identity.SendVerifyCodeRsp{}
	err := HTTP.DoNoAuth("/service/identity/send_verify_code", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	assert.NotEmpty(t, rsp.VerifyCodeId)
}

func TestSendVerifyCode_InvalidEmail_Error(t *testing.T) {
	req := &identity.SendVerifyCodeReq{
		RequestId: client.NewRequestID(),
		Destination: &identity.SendVerifyCodeReq_Email{
			Email: "not-an-email",
		},
	}
	rsp := &identity.SendVerifyCodeRsp{}
	err := HTTP.DoNoAuth("/service/identity/send_verify_code", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(9004), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 5. RefreshToken
// ---------------------------------------------------------------------------

func TestRefreshToken_Success(t *testing.T) {
	username := randUser()
	password := "test123456"

	// Register.
	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", regReq, regRsp)
	require.NoError(t, err)
	require.True(t, regRsp.Header.Success)

	oldAccessToken := regRsp.Tokens.AccessToken
	oldRefreshToken := regRsp.Tokens.RefreshToken

	// Refresh.
	refreshReq := &identity.RefreshTokenReq{
		RequestId:    client.NewRequestID(),
		RefreshToken: oldRefreshToken,
	}
	refreshRsp := &identity.RefreshTokenRsp{}
	err = HTTP.DoNoAuth("/service/identity/refresh_token", refreshReq, refreshRsp)
	require.NoError(t, err)
	require.NotNil(t, refreshRsp.Header)
	assert.True(t, refreshRsp.Header.Success)
	require.NotNil(t, refreshRsp.Tokens)
	assert.NotEmpty(t, refreshRsp.Tokens.AccessToken)
	assert.NotEqual(t, oldAccessToken, refreshRsp.Tokens.AccessToken)
	assert.NotEmpty(t, refreshRsp.Tokens.RefreshToken)
	assert.NotEqual(t, oldRefreshToken, refreshRsp.Tokens.RefreshToken)
}

func TestRefreshToken_Reuse_Error(t *testing.T) {
	username := randUser()
	password := "test123456"

	regReq := &identity.RegisterReq{
		RequestId: client.NewRequestID(),
		Credential: &identity.RegisterReq_UsernamePwd{
			UsernamePwd: &identity.UsernamePassword{
				Username: username,
				Password: password,
			},
		},
		Nickname: username,
	}
	regRsp := &identity.RegisterRsp{}
	err := HTTP.DoNoAuth("/service/identity/register", regReq, regRsp)
	require.NoError(t, err)
	require.True(t, regRsp.Header.Success)

	refreshToken := regRsp.Tokens.RefreshToken

	// First refresh — succeeds.
	refreshReq1 := &identity.RefreshTokenReq{
		RequestId:    client.NewRequestID(),
		RefreshToken: refreshToken,
	}
	refreshRsp1 := &identity.RefreshTokenRsp{}
	err = HTTP.DoNoAuth("/service/identity/refresh_token", refreshReq1, refreshRsp1)
	require.NoError(t, err)
	require.True(t, refreshRsp1.Header.Success)

	// Second refresh with the SAME old token — error_code=1008.
	refreshReq2 := &identity.RefreshTokenReq{
		RequestId:    client.NewRequestID(),
		RefreshToken: refreshToken,
	}
	refreshRsp2 := &identity.RefreshTokenRsp{}
	err = HTTP.DoNoAuth("/service/identity/refresh_token", refreshReq2, refreshRsp2)
	require.NoError(t, err)
	require.NotNil(t, refreshRsp2.Header)
	assert.False(t, refreshRsp2.Header.Success)
	assert.Equal(t, int32(1008), refreshRsp2.Header.ErrorCode)
}

func TestRefreshToken_Invalid_Error(t *testing.T) {
	refreshReq := &identity.RefreshTokenReq{
		RequestId:    client.NewRequestID(),
		RefreshToken: "invalid-refresh-token-string",
	}
	refreshRsp := &identity.RefreshTokenRsp{}
	err := HTTP.DoNoAuth("/service/identity/refresh_token", refreshReq, refreshRsp)
	require.NoError(t, err)
	require.NotNil(t, refreshRsp.Header)
	assert.False(t, refreshRsp.Header.Success)
	assert.Equal(t, int32(1003), refreshRsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 6. GetProfile
// ---------------------------------------------------------------------------

func TestGetProfile_Self_Success(t *testing.T) {
	authed, username, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &identity.GetProfileReq{
		RequestId: client.NewRequestID(),
	}
	rsp := &identity.GetProfileRsp{}
	err := authed.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, authed.UserID, rsp.UserInfo.UserId)
	assert.Equal(t, username, rsp.UserInfo.Nickname)
}

func TestGetProfile_OtherUser_Success(t *testing.T) {
	authed1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	authed2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	// authed1 queries authed2's profile.
	req := &identity.GetProfileReq{
		RequestId: client.NewRequestID(),
		UserId:    &authed2.UserID,
	}
	rsp := &identity.GetProfileRsp{}
	err := authed1.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, authed2.UserID, rsp.UserInfo.UserId)
}

func TestGetProfile_NotFound_Error(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	nonexistentID := "nonexistent_user_id_12345"
	req := &identity.GetProfileReq{
		RequestId: client.NewRequestID(),
		UserId:    &nonexistentID,
	}
	rsp := &identity.GetProfileRsp{}
	err := authed.DoAuth("/service/identity/get_profile", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.False(t, rsp.Header.Success)
	assert.Equal(t, int32(1004), rsp.Header.ErrorCode)
}

// ---------------------------------------------------------------------------
// 7. UpdateProfile
// ---------------------------------------------------------------------------

func TestUpdateProfile_Nickname_Success(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)

	newNickname := "updated_nickname_value"
	req := &identity.UpdateProfileReq{
		RequestId: client.NewRequestID(),
		Nickname:  &newNickname,
	}
	rsp := &identity.UpdateProfileRsp{}
	err := authed.DoAuth("/service/identity/update_profile", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	require.NotNil(t, rsp.UserInfo)
	assert.Equal(t, newNickname, rsp.UserInfo.Nickname)
}

func TestUpdateProfile_NoToken_Error(t *testing.T) {
	newNickname := "some_nick"
	req := &identity.UpdateProfileReq{
		RequestId: client.NewRequestID(),
		Nickname:  &newNickname,
	}
	rsp := &identity.UpdateProfileRsp{}
	err := HTTP.DoNoAuth("/service/identity/update_profile", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.False(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 8. SearchUsers
// ---------------------------------------------------------------------------

func TestSearchUsers_Success(t *testing.T) {
	authed, username, _ := fixture.RegisterAndLogin(t, HTTP)

	// Search by a substring of the registered username.
	searchKey := username[:len(username)/2]
	req := &identity.SearchUsersReq{
		RequestId: client.NewRequestID(),
		SearchKey: searchKey,
	}
	rsp := &identity.SearchUsersRsp{}
	err := authed.DoAuth("/service/identity/search_users", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
}

// ---------------------------------------------------------------------------
// 9. GetMultiUserInfo
// ---------------------------------------------------------------------------

func TestGetMultiUserInfo_Success(t *testing.T) {
	authed1, _, _ := fixture.RegisterAndLogin(t, HTTP)
	authed2, _, _ := fixture.RegisterAndLogin(t, HTTP)

	req := &identity.GetMultiUserInfoReq{
		RequestId: client.NewRequestID(),
		UsersId:   []string{authed1.UserID, authed2.UserID},
	}
	rsp := &identity.GetMultiUserInfoRsp{}
	err := authed1.DoAuth("/service/identity/get_multi_info", req, rsp)
	require.NoError(t, err)
	require.NotNil(t, rsp.Header)
	assert.True(t, rsp.Header.Success)
	assert.Len(t, rsp.UsersInfo, 2)
	assert.Contains(t, rsp.UsersInfo, authed1.UserID)
	assert.Contains(t, rsp.UsersInfo, authed2.UserID)
}
