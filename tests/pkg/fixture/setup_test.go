// NOTE: Tests require full docker-compose stack running.
//go:build func

package fixture

import (
	"os"
	"testing"

	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/client"
)

var HTTP *client.HTTPClient

func TestMain(m *testing.M) {
	cfg := client.LoadConfig("../../config.yaml")
	HTTP = client.NewHTTPClient(cfg)
	if err := cleanup.WaitForStackReady(cfg, 120*1e9); err != nil {
		panic(err)
	}
	cleanup.CleanupAll(nil, cfg)
	os.Exit(m.Run())
}
