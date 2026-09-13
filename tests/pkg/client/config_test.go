package client

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/require"
)

func TestComposeDirectoryIsRelativeToConfigFile(t *testing.T) {
	t.Setenv("COMPOSE_DIR", "")
	root := t.TempDir()
	configDir := filepath.Join(root, "tests")
	require.NoError(t, os.Mkdir(configDir, 0700))
	path := filepath.Join(configDir, "config.yaml")
	require.NoError(t, os.WriteFile(path, []byte("infra:\n  compose_dir: ..\n"), 0600))
	require.Equal(t, root, LoadConfig(path).Infra.ComposeDir)
}
