package fixture

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"os"
	"strings"
	"testing"
	"time"
)

// JWTVariant describes a negative token issued only for an isolated synthetic stack.
type JWTVariant struct {
	ExpiresAt       time.Time
	IssuedAt        time.Time
	NotBefore       time.Time
	WrongSigningKey bool
	UnknownKeyID    bool
}

// SignJWTVariant preserves a fixture account's claims while changing validity.
// CHATNOW_JWT_CONFIG must be the synthetic configuration used by the test stack.
// Errors deliberately omit all token, key, and decoded claim values.
func SignJWTVariant(t testing.TB, token string, variant JWTVariant) string {
	t.Helper()
	var config struct {
		Auth struct {
			JWT struct {
				Keys map[string]string `json:"keys"`
			} `json:"jwt"`
		} `json:"auth"`
	}
	if json.Unmarshal([]byte(os.Getenv("CHATNOW_JWT_CONFIG")), &config) != nil {
		t.Fatal("synthetic CHATNOW_JWT_CONFIG is required for signed JWT boundary tests")
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		t.Fatal("fixture token is malformed")
	}
	decode := func(part string) map[string]any {
		data, err := base64.RawURLEncoding.DecodeString(part)
		if err != nil {
			t.Fatal("fixture token encoding is malformed")
		}
		var value map[string]any
		decoder := json.NewDecoder(strings.NewReader(string(data)))
		decoder.UseNumber()
		if decoder.Decode(&value) != nil || value == nil {
			t.Fatal("fixture token JSON is malformed")
		}
		return value
	}
	header, claims := decode(parts[0]), decode(parts[1])
	kid, ok := header["kid"].(string)
	key := []byte(config.Auth.JWT.Keys[kid])
	if !ok || header["alg"] != "HS256" || len(key) < 32 {
		t.Fatal("fixture token does not match the synthetic HS256 configuration")
	}
	if variant.UnknownKeyID {
		header["kid"] = "unrecognized-test-key"
	}
	if variant.WrongSigningKey {
		key = make([]byte, 32)
		if _, err := rand.Read(key); err != nil {
			t.Fatal("cannot create synthetic invalid signing key")
		}
	}
	claims["exp"] = variant.ExpiresAt.Unix()
	claims["iat"] = variant.IssuedAt.Unix()
	if !variant.NotBefore.IsZero() {
		claims["nbf"] = variant.NotBefore.Unix()
	}
	encode := func(value map[string]any) string {
		data, err := json.Marshal(value)
		if err != nil {
			t.Fatal("cannot encode synthetic JWT variant")
		}
		return base64.RawURLEncoding.EncodeToString(data)
	}
	unsigned := encode(header) + "." + encode(claims)
	mac := hmac.New(sha256.New, key)
	_, _ = mac.Write([]byte(unsigned))
	return unsigned + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}
