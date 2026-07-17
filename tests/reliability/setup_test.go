//go:build reliability

package reliability_test

import (
	"os"
	"testing"

	"chatnow-tests/pkg/client"
)

var HTTP *client.HTTPClient

func TestMain(m *testing.M) {
	cfg := client.LoadConfig("../config.yaml")
	HTTP = client.NewHTTPClient(cfg)
	os.Exit(m.Run())
}
