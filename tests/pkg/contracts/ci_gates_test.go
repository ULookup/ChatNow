package contracts

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
	"gopkg.in/yaml.v3"
)

type workflowContract struct {
	On struct {
		PullRequest map[string]any   `yaml:"pull_request"`
		Schedule    []map[string]any `yaml:"schedule"`
	} `yaml:"on"`
	Jobs map[string]workflowJob `yaml:"jobs"`
}

type workflowJob struct {
	If    string            `yaml:"if"`
	Env   map[string]string `yaml:"env"`
	Needs any               `yaml:"needs"`
	Steps []workflowStep    `yaml:"steps"`
}

type workflowStep struct {
	Uses string            `yaml:"uses"`
	Run  string            `yaml:"run"`
	If   string            `yaml:"if"`
	Env  map[string]string `yaml:"env"`
}

type composeContract struct {
	Services map[string]struct {
		Entrypoint string `yaml:"entrypoint"`
	} `yaml:"services"`
}

func TestCIGates(t *testing.T) {
	root := repositoryRoot(t)
	workflowBytes, err := os.ReadFile(filepath.Join(root, ".github/workflows/ci.yml"))
	require.NoError(t, err)
	composeBytes, err := os.ReadFile(filepath.Join(root, "docker-compose.yml"))
	require.NoError(t, err)

	var workflow workflowContract
	require.NoError(t, yaml.Unmarshal(workflowBytes, &workflow), "workflow must be valid YAML")
	require.NotNil(t, workflow.On.PullRequest, "workflow must handle pull requests")
	require.NotEmpty(t, workflow.On.Schedule, "workflow must define a schedule")

	reliability, ok := workflow.Jobs["reliability"]
	require.True(t, ok, "RL-05 must have a dedicated reliability job")
	require.Nil(t, reliability.Needs, "reliability must own its setup instead of depending on another job")
	require.Contains(t, reliability.If, "github.event_name == 'pull_request'")
	require.Contains(t, reliability.If, "github.event_name == 'schedule'")
	assertFullStackGateJob(t, reliability, "make test-reliability")

	perfCache, ok := workflow.Jobs["perf-cache"]
	require.True(t, ok, "PF-09 must have a dedicated perf-cache job")
	require.Nil(t, perfCache.Needs, "perf-cache must own its setup instead of depending on another job")
	require.Contains(t, perfCache.If, "github.event_name == 'schedule'")
	assertFullStackGateJob(t, perfCache, "make test-perf-cache-gate")
	require.NotContains(t, allRuns(perfCache), "make test-perf-cache\n", "PF-09 CI must not use the skip-capable discovery target")

	userLimit := gateEnv(t, perfCache, "TRANSMITE_RATE_LIMIT_USER_MAX")
	sessionLimit := gateEnv(t, perfCache, "TRANSMITE_RATE_LIMIT_SESSION_MAX")
	require.Greater(t, userLimit, 50000, "PF-09 user limit must exceed its 5000 msg/s ten-second load")
	require.Greater(t, sessionLimit, 50000, "PF-09 session limit must exceed its 5000 msg/s ten-second load")

	var compose composeContract
	require.NoError(t, yaml.Unmarshal(composeBytes, &compose), "Compose file must be valid YAML")
	transmite, ok := compose.Services["transmite_server"]
	require.True(t, ok)
	require.Contains(t, transmite.Entrypoint, "-rate_limit_user_max=${TRANSMITE_RATE_LIMIT_USER_MAX:-600}")
	require.Contains(t, transmite.Entrypoint, "-rate_limit_session_max=${TRANSMITE_RATE_LIMIT_SESSION_MAX:-3000}")
}

func assertFullStackGateJob(t *testing.T, job workflowJob, target string) {
	t.Helper()
	runs := allRuns(job)
	require.Contains(t, uses(job), "actions/checkout@v4")
	require.Contains(t, uses(job), "actions/setup-go@v5")
	for _, command := range []string{
		"protobuf-compiler", "protoc-gen-go", "docker compose up -d --build",
		"wait_for_services.sh", "make proto", "go mod download", target,
	} {
		require.Contains(t, runs, command)
	}
	require.True(t, hasAlwaysTeardown(job), "gate job must always tear down its own stack")
}

func allRuns(job workflowJob) string {
	var runs strings.Builder
	for _, step := range job.Steps {
		runs.WriteString(step.Run)
		runs.WriteByte('\n')
	}
	return runs.String()
}

func uses(job workflowJob) string {
	var values []string
	for _, step := range job.Steps {
		values = append(values, step.Uses)
	}
	return strings.Join(values, "\n")
}

func hasAlwaysTeardown(job workflowJob) bool {
	for _, step := range job.Steps {
		if step.If == "always()" && strings.Contains(step.Run, "docker compose down -v") {
			return true
		}
	}
	return false
}

func gateEnv(t *testing.T, job workflowJob, name string) int {
	t.Helper()
	raw := job.Env[name]
	if raw == "" {
		for _, step := range job.Steps {
			if strings.Contains(step.Run, "docker compose up -d --build") && step.Env[name] != "" {
				raw = step.Env[name]
				break
			}
		}
	}
	require.NotEmpty(t, raw, "%s must be supplied to the PF-09 stack", name)
	value, err := strconv.Atoi(raw)
	require.NoError(t, err, "%s must be an explicit integer", name)
	return value
}

func repositoryRoot(t *testing.T) string {
	t.Helper()
	_, filename, _, ok := runtime.Caller(0)
	require.True(t, ok)
	return filepath.Clean(filepath.Join(filepath.Dir(filename), "..", "..", ".."))
}
