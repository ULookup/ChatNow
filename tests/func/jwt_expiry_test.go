//go:build func

package func_test

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"chatnow-tests/pkg/client"
	"chatnow-tests/pkg/fixture"
	identity "chatnow-tests/proto/chatnow/identity"
)

// FN-AM-07 | P1 | Signed expired JWTs remain distinguishable from invalid JWTs.
func TestFN_AM_ExpiredJWTClassification(t *testing.T) {
	authed, _, _ := fixture.RegisterAndLogin(t, HTTP)
	now := time.Now()
	cases := []struct {
		name    string
		variant fixture.JWTVariant
		code    int32
	}{
		{"expired_30_seconds", fixture.JWTVariant{ExpiresAt: now.Add(-30 * time.Second), IssuedAt: now.Add(-time.Hour)}, 1002},
		{"expired_120_seconds", fixture.JWTVariant{ExpiresAt: now.Add(-120 * time.Second), IssuedAt: now.Add(-time.Hour)}, 1002},
		{"expired_one_day", fixture.JWTVariant{ExpiresAt: now.Add(-24 * time.Hour), IssuedAt: now.Add(-48 * time.Hour)}, 1002},
		{"expired_wrong_signature", fixture.JWTVariant{ExpiresAt: now.Add(-24 * time.Hour), IssuedAt: now.Add(-48 * time.Hour), WrongSigningKey: true}, 1003},
		{"expired_unknown_key", fixture.JWTVariant{ExpiresAt: now.Add(-24 * time.Hour), IssuedAt: now.Add(-48 * time.Hour), UnknownKeyID: true}, 1003},
		{"future_issued_at", fixture.JWTVariant{ExpiresAt: now.Add(time.Hour), IssuedAt: now.Add(5 * time.Minute)}, 1003},
		{"future_not_before", fixture.JWTVariant{ExpiresAt: now.Add(time.Hour), IssuedAt: now.Add(-time.Hour), NotBefore: now.Add(5 * time.Minute)}, 1003},
		{"expired_future_issued_at", fixture.JWTVariant{ExpiresAt: now.Add(-24 * time.Hour), IssuedAt: now.Add(5 * time.Minute)}, 1003},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			refresh := fixture.SignJWTVariant(t, authed.RefreshToken, tc.variant)
			rsp := &identity.RefreshTokenRsp{}
			err := HTTP.DoNoAuth("/service/identity/refresh_token", &identity.RefreshTokenReq{
				RequestId: client.NewRequestID(), RefreshToken: refresh,
			}, rsp)
			require.NoError(t, err)
			require.NotNil(t, rsp.Header)
			assert.False(t, rsp.Header.Success)
			assert.Equal(t, tc.code, rsp.Header.ErrorCode, "Identity must distinguish expiration from invalid verification")
			assert.True(t, rsp.Tokens == nil, "a rejected token must never produce new credentials")

			access := fixture.SignJWTVariant(t, authed.AccessToken, tc.variant)
			profile := &identity.GetProfileRsp{}
			err = HTTP.Do("/service/identity/get_profile", &identity.GetProfileReq{RequestId: client.NewRequestID()}, profile, access)
			require.Error(t, err, "Gateway must reject the invalid token")
			assert.ErrorContains(t, err, "http status 401:")
			if tc.code == 1002 {
				assert.ErrorContains(t, err, "token expired", "Gateway must preserve the expiration reason")
			}
		})
	}
	t.Run("valid_tokens_still_work", func(t *testing.T) {
		profile := &identity.GetProfileRsp{}
		require.NoError(t, authed.DoAuth("/service/identity/get_profile", &identity.GetProfileReq{RequestId: client.NewRequestID()}, profile))
		require.True(t, profile.Header.Success)
		rsp := &identity.RefreshTokenRsp{}
		require.NoError(t, HTTP.DoNoAuth("/service/identity/refresh_token", &identity.RefreshTokenReq{
			RequestId: client.NewRequestID(), RefreshToken: authed.RefreshToken,
		}, rsp))
		require.NotNil(t, rsp.Header)
		require.True(t, rsp.Header.Success)
	})
}
