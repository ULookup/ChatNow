//go:build bvt

package bvt_test

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
