package contracts

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
	"gopkg.in/yaml.v3"
)

func TestAllWorkflowsParse(t *testing.T) {
	files, err := filepath.Glob(filepath.Join(repositoryRoot(t), ".github/workflows/*.yml"))
	require.NoError(t, err)
	for _, file := range files {
		t.Run(filepath.Base(file), func(t *testing.T) {
			data, err := os.ReadFile(file)
			require.NoError(t, err)
			var workflow map[string]any
			require.NoError(t, yaml.Unmarshal(data, &workflow))
		})
	}
}

func TestSyntheticEnvironmentDoesNotOverwriteExistingState(t *testing.T) {
	root := t.TempDir()
	require.NoError(t, os.Mkdir(filepath.Join(root, "scripts"), 0700))
	for _, relative := range []string{"scripts/create_test_env.py", ".env.example"} {
		data, err := os.ReadFile(filepath.Join(repositoryRoot(t), relative))
		require.NoError(t, err)
		require.NoError(t, os.WriteFile(filepath.Join(root, relative), data, 0600))
	}
	run := func() ([]byte, error) {
		return exec.Command("python3", filepath.Join(root, "scripts/create_test_env.py")).CombinedOutput()
	}
	output, err := run()
	require.NoError(t, err, string(output))
	before, err := os.ReadFile(filepath.Join(root, ".env"))
	require.NoError(t, err)
	for _, line := range strings.Split(strings.TrimSpace(string(before)), "\n") {
		parts := strings.SplitN(line, "=", 2)
		require.Len(t, parts, 2)
		require.NotContains(t, string(output), strings.Trim(parts[1], "'"))
	}
	_, err = run()
	require.Error(t, err, "existing credentials must not be rotated")
	after, err := os.ReadFile(filepath.Join(root, ".env"))
	require.NoError(t, err)
	require.True(t, string(before) == string(after), "existing credentials changed")
	require.NoError(t, os.Remove(filepath.Join(root, ".env")))
	require.NoError(t, os.MkdirAll(filepath.Join(root, "middle/data"), 0700))
	_, err = run()
	require.Error(t, err, "existing persistent data must not receive new credentials")
}

func TestRuntimeGatesInitializeSyntheticEnvironment(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), ".github/workflows/ci.yml"))
	require.NoError(t, err)
	var workflow workflowContract
	require.NoError(t, yaml.Unmarshal(data, &workflow))
	for name, job := range workflow.Jobs {
		for index, step := range job.Steps {
			if strings.TrimSpace(step.Run) == "docker compose up -d --build" {
				require.Greater(t, index, 0)
				require.Equal(t, "python3 scripts/create_test_env.py --github-env", strings.TrimSpace(job.Steps[index-1].Run), name)
			}
		}
	}
}
