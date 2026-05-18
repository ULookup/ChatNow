//go:build func

package func_test

import (
	"os"
	"testing"

	"chatnow-tests/pkg/client"
)

var HTTP *client.HTTPClient

func TestMain(m *testing.M) {
	cfg := client.LoadConfig("")
	HTTP = client.NewHTTPClient(cfg)
	os.Exit(m.Run())
}
