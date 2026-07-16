package contracts

import (
	"os"
	"path/filepath"
	"runtime"
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
	assertDecoratedCommandsDoNotSatisfyGate(t)
	assertContractsRunInBuild(t, workflow.Jobs["build"])

	reliability, ok := workflow.Jobs["reliability"]
	require.True(t, ok, "RL-05 must have a dedicated reliability job")
	require.Nil(t, reliability.Needs, "reliability must own its setup instead of depending on another job")
	require.Equal(t, "github.event_name == 'pull_request' || github.event_name == 'schedule'", reliability.If)
	assertFullStackGateJob(t, reliability, "cd tests && make test-reliability")

	perfCache, ok := workflow.Jobs["perf-cache"]
	require.True(t, ok, "PF-09 must have a dedicated perf-cache job")
	require.Nil(t, perfCache.Needs, "perf-cache must own its setup instead of depending on another job")
	require.Equal(t, "github.event_name == 'schedule'", perfCache.If)
	assertFullStackGateJob(t, perfCache, "cd tests && make test-perf-cache-gate")
	assertTargetAbsent(t, perfCache, "test-perf-cache")

	require.Equal(t, "2147483647", gateEnv(t, perfCache, "TRANSMITE_RATE_LIMIT_USER_MAX"))
	require.Equal(t, "2147483647", gateEnv(t, perfCache, "TRANSMITE_RATE_LIMIT_SESSION_MAX"))

	var compose composeContract
	require.NoError(t, yaml.Unmarshal(composeBytes, &compose), "Compose file must be valid YAML")
	transmite, ok := compose.Services["transmite_server"]
	require.True(t, ok)
	require.Contains(t, transmite.Entrypoint, "-rate_limit_user_max=${TRANSMITE_RATE_LIMIT_USER_MAX:-600}")
	require.Contains(t, transmite.Entrypoint, "-rate_limit_session_max=${TRANSMITE_RATE_LIMIT_SESSION_MAX:-3000}")
}

func assertFullStackGateJob(t *testing.T, job workflowJob, target string) {
	t.Helper()
	require.NotEqual(t, -1, exactUsesStepIndex(job, "actions/checkout@v4"))
	require.NotEqual(t, -1, exactUsesStepIndex(job, "actions/setup-go@v5"))
	for _, command := range []string{
		"sudo apt-get install -y protobuf-compiler netcat-openbsd",
		"go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11",
		"docker compose up -d --build", "./scripts/wait_for_services.sh",
		"cd tests && make proto", "cd tests && go mod download", target,
	} {
		require.NotEqual(t, -1, exactRunStepIndex(job, command), "missing exact executable command %q", command)
	}
	require.True(t, hasAlwaysTeardown(job), "gate job must always tear down its own stack")
}

func assertContractsRunInBuild(t *testing.T, build workflowJob) {
	t.Helper()
	setupGo := exactUsesStepIndex(build, "actions/setup-go@v5")
	proto := exactRunStepIndex(build, "cd tests && make proto")
	deps := exactRunStepIndex(build, "cd tests && go mod download")
	contracts := exactRunStepIndex(build, "cd tests && go test ./pkg/contracts -count=1")
	require.NotEqual(t, -1, setupGo, "build must set up Go")
	require.Greater(t, proto, setupGo, "protobuf generation must follow Go setup")
	require.Greater(t, deps, proto, "dependency download must follow protobuf generation")
	require.Greater(t, contracts, deps, "CI contract tests must run after setup, protobuf generation, and dependency download")
}

func exactRunStepIndex(job workflowJob, wanted string) int {
	for index, step := range job.Steps {
		for _, command := range executableCommands(step.Run) {
			if command == wanted {
				return index
			}
		}
	}
	return -1
}

func exactUsesStepIndex(job workflowJob, wanted string) int {
	for index, step := range job.Steps {
		if step.Uses == wanted {
			return index
		}
	}
	return -1
}

func assertDecoratedCommandsDoNotSatisfyGate(t *testing.T) {
	t.Helper()
	const gate = "cd tests && make test-perf-cache-gate"
	require.Equal(t, 0, exactRunStepIndex(workflowJob{Steps: []workflowStep{{Run: gate}}}, gate))
	for _, lookalike := range []string{
		"# " + gate,
		"echo '" + gate + "'",
		gate + "-disabled",
		"false && " + gate,
		gate + " # disabled",
	} {
		job := workflowJob{Steps: []workflowStep{{Run: lookalike}}}
		require.Equal(t, -1, exactRunStepIndex(job, gate), "%q must not satisfy the executable gate contract", lookalike)
	}
}

func executableCommands(script string) []string {
	var commands []string
	for _, line := range strings.Split(script, "\n") {
		line = strings.TrimSpace(line)
		if line != "" && !strings.HasPrefix(line, "#") {
			commands = append(commands, line)
		}
	}
	return commands
}

func assertTargetAbsent(t *testing.T, job workflowJob, target string) {
	t.Helper()
	for _, step := range job.Steps {
		for _, command := range executableCommands(step.Run) {
			words := strings.FieldsFunc(command, func(r rune) bool {
				return strings.ContainsRune(" \t;&|()<>#", r)
			})
			require.NotContains(t, words, target, "PF-09 CI must not execute the skip-capable discovery target")
		}
	}
}

func hasAlwaysTeardown(job workflowJob) bool {
	for _, step := range job.Steps {
		if step.If == "always()" && len(executableCommands(step.Run)) == 1 && executableCommands(step.Run)[0] == "docker compose down -v" {
			return true
		}
	}
	return false
}

func gateEnv(t *testing.T, job workflowJob, name string) string {
	t.Helper()
	raw := job.Env[name]
	if raw == "" {
		for _, step := range job.Steps {
			if len(executableCommands(step.Run)) == 1 && executableCommands(step.Run)[0] == "docker compose up -d --build" && step.Env[name] != "" {
				raw = step.Env[name]
				break
			}
		}
	}
	require.NotEmpty(t, raw, "%s must be supplied to the PF-09 stack", name)
	return raw
}

func repositoryRoot(t *testing.T) string {
	t.Helper()
	_, filename, _, ok := runtime.Caller(0)
	require.True(t, ok)
	return filepath.Clean(filepath.Join(filepath.Dir(filename), "..", "..", ".."))
}
