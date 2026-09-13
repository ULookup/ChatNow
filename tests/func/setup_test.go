// NOTE: Tests require full docker-compose stack running. See docs/superpowers/plans/2026-07-09-phase1-bvt-core-and-infra.md
//go:build func

package func_test

import (
	"os"
	"testing"

	"chatnow-tests/pkg/cleanup"
	"chatnow-tests/pkg/client"
)

var HTTP *client.HTTPClient
var Cfg *client.Config

func TestMain(m *testing.M) {
	Cfg = client.LoadConfig("../config.yaml")
	HTTP = client.NewHTTPClient(Cfg)
	if err := cleanup.WaitForStackReady(Cfg, 120*1e9); err != nil {
		panic(err)
	}
	cleanup.CleanupAll(nil, Cfg)
	os.Exit(m.Run())
}
