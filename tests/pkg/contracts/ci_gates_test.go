package contracts

import (
	"fmt"
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
	assertInvalidGateJobsRejected(t, perfCache, "cd tests && make test-perf-cache-gate")

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
	require.NoError(t, validateFullStackGateJob(job, target))
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
		if strings.TrimSpace(step.Run) == wanted {
			return index
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
		"exit 0\n" + gate,
		"if false; then " + gate + "; fi",
		gate + " # disabled",
		gate + "\nexit 0",
		"cat <<'EOF'\n" + gate + "\nEOF",
		"gate() { " + gate + "; }\ngate",
		`cd tests && make "test-perf-cache"`,
	} {
		job := workflowJob{Steps: []workflowStep{{Run: lookalike}}}
		require.Equal(t, -1, exactRunStepIndex(job, gate), "%q must not satisfy the executable gate contract", lookalike)
	}
}

func assertInvalidGateJobsRejected(t *testing.T, valid workflowJob, target string) {
	t.Helper()
	gate := exactRunStepIndex(valid, target)
	deps := exactRunStepIndex(valid, "cd tests && go mod download")
	teardown := exactRunStepIndex(valid, "docker compose down -v")
	require.NotEqual(t, -1, gate)
	require.NotEqual(t, -1, deps)
	require.NotEqual(t, -1, teardown)

	for name, mutate := range map[string]func(*workflowJob){
		"exit zero after gate": func(job *workflowJob) {
			job.Steps[gate].Run = target + "\nexit 0"
		},
		"if false gate": func(job *workflowJob) {
			job.Steps[gate].Run = "if false; then " + target + "; fi"
		},
		"quoted skip target": func(job *workflowJob) {
			job.Steps[gate].Run = `cd tests && make "test-perf-cache"`
		},
		"gate before dependencies": func(job *workflowJob) {
			job.Steps[gate], job.Steps[deps] = job.Steps[deps], job.Steps[gate]
		},
		"teardown before gate": func(job *workflowJob) {
			job.Steps[gate], job.Steps[teardown] = job.Steps[teardown], job.Steps[gate]
		},
		"teardown not last": func(job *workflowJob) {
			job.Steps = append(job.Steps, workflowStep{Run: "true"})
		},
	} {
		t.Run(name, func(t *testing.T) {
			invalid := cloneWorkflowJob(valid)
			mutate(&invalid)
			require.Error(t, validateFullStackGateJob(invalid, target))
		})
	}
}

func cloneWorkflowJob(job workflowJob) workflowJob {
	clone := job
	clone.Steps = append([]workflowStep(nil), job.Steps...)
	return clone
}

func assertTargetAbsent(t *testing.T, job workflowJob, target string) {
	t.Helper()
	for _, step := range job.Steps {
		require.NotEqual(t, "cd tests && make "+target, strings.TrimSpace(step.Run),
			"PF-09 CI must not execute the skip-capable discovery target")
	}
}

func validateFullStackGateJob(job workflowJob, target string) error {
	const install = "sudo apt-get update\nsudo apt-get install -y protobuf-compiler netcat-openbsd"
	ordered := []struct {
		label string
		index int
	}{
		{"checkout", exactUsesStepIndex(job, "actions/checkout@v4")},
		{"Go setup", exactUsesStepIndex(job, "actions/setup-go@v5")},
		{"system dependency install", exactRunStepIndex(job, install)},
		{"protoc generator install", exactRunStepIndex(job, "go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11")},
		{"full-stack startup", exactRunStepIndex(job, "docker compose up -d --build")},
		{"service wait", exactRunStepIndex(job, "./scripts/wait_for_services.sh")},
		{"protobuf generation", exactRunStepIndex(job, "cd tests && make proto")},
		{"dependency download", exactRunStepIndex(job, "cd tests && go mod download")},
		{"gate", exactRunStepIndex(job, target)},
		{"teardown", exactRunStepIndex(job, "docker compose down -v")},
	}
	previous := -1
	for _, step := range ordered {
		if step.index < 0 {
			return fmt.Errorf("missing exact %s step", step.label)
		}
		if step.index <= previous {
			return fmt.Errorf("%s step is out of order", step.label)
		}
		previous = step.index
	}
	teardown := ordered[len(ordered)-1].index
	if teardown != len(job.Steps)-1 {
		return fmt.Errorf("teardown must be the final step")
	}
	if job.Steps[teardown].If != "always()" {
		return fmt.Errorf("teardown must use if: always()")
	}
	if countExactRunSteps(job, target) != 1 {
		return fmt.Errorf("gate command must appear exactly once")
	}
	if countExactRunSteps(job, "docker compose down -v") != 1 {
		return fmt.Errorf("teardown command must appear exactly once")
	}
	return nil
}

func countExactRunSteps(job workflowJob, wanted string) int {
	count := 0
	for _, step := range job.Steps {
		if strings.TrimSpace(step.Run) == wanted {
			count++
		}
	}
	return count
}

func gateEnv(t *testing.T, job workflowJob, name string) string {
	t.Helper()
	raw := job.Env[name]
	if raw == "" {
		for _, step := range job.Steps {
			if strings.TrimSpace(step.Run) == "docker compose up -d --build" && step.Env[name] != "" {
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
