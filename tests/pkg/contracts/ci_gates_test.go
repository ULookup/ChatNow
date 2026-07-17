package contracts

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"chatnow-tests/pkg/client"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/wrapperspb"
	"gopkg.in/yaml.v3"
)

func TestDirectProtobufHTTPClient(t *testing.T) {
	received := make(chan string, 1)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		request := &wrapperspb.StringValue{}
		if err := proto.Unmarshal(body, request); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		received <- fmt.Sprintf("%s|%s|%s", r.URL.Path, r.Header.Get("Content-Type"), request.GetValue())
		response, err := proto.Marshal(wrapperspb.String("direct-response"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/x-protobuf")
		_, _ = w.Write(response)
	}))
	t.Cleanup(server.Close)

	httpClient := client.NewHTTPClient(&client.Config{})
	response := &wrapperspb.StringValue{}
	require.NoError(t, httpClient.DoProtobufURL(
		server.URL+"/chatnow.push.PushService/PushToUser",
		wrapperspb.String("direct-request"), response))
	require.Equal(t, "direct-response", response.GetValue())
	require.Equal(t,
		"/chatnow.push.PushService/PushToUser|application/x-protobuf|direct-request",
		<-received)
}

func TestGoCacheRegressionsReplaceTemporaryCPP(t *testing.T) {
	root := repositoryRoot(t)
	for _, relativePath := range []string{
		"common/test/test_user_info_generation_fence.cc",
		"common/test/test_unacked_pending_ledger.cc",
	} {
		_, err := os.Stat(filepath.Join(root, relativePath))
		require.ErrorIs(t, err, os.ErrNotExist, "%s must be removed", relativePath)
	}

	cmake, err := os.ReadFile(filepath.Join(root, "common/test/CMakeLists.txt"))
	require.NoError(t, err)
	require.NotContains(t, string(cmake), "test_unacked_pending_ledger",
		"the temporary C++ Unacked target must be removed")

	cacheTestPath := filepath.Join(root, "tests/func/cache_test.go")
	parsed, err := parser.ParseFile(token.NewFileSet(), cacheTestPath, nil, 0)
	require.NoError(t, err)
	var found bool
	for _, declaration := range parsed.Decls {
		function, ok := declaration.(*ast.FuncDecl)
		if ok && function.Name.Name == "TestFN_CA_UnackedSameUserSeqLatestPayloadAndAck" {
			found = true
			break
		}
	}
	require.True(t, found,
		"the Go functional suite must own the same-user_seq latest-payload/ACK regression")
}

func TestReadAckUsesConversationSequenceWatermark(t *testing.T) {
	root := repositoryRoot(t)
	serviceProto, err := os.ReadFile(filepath.Join(root, "proto/message/message_service.proto"))
	require.NoError(t, err)
	require.Contains(t, string(serviceProto), "uint64 seq_id = 3;")
	require.NotContains(t, string(serviceProto), "uint64 message_id = 3;")

	server, err := os.ReadFile(filepath.Join(root, "message/source/message_server.h"))
	require.NoError(t, err)
	updateReadAck := string(server)
	start := strings.Index(updateReadAck, "void UpdateReadAck(")
	require.GreaterOrEqual(t, start, 0)
	end := strings.Index(updateReadAck[start:], "\n    // ====== MQ consumer")
	require.Greater(t, end, 0)
	updateReadAck = updateReadAck[start : start+end]
	require.Contains(t, updateReadAck, "req->seq_id()")
	require.Contains(t, updateReadAck, "update_last_ack_seq(")
	require.NotContains(t, updateReadAck, "req->message_id()")
	require.NotContains(t, updateReadAck, "select_by_id(")
}

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
	Uses            string            `yaml:"uses"`
	Run             string            `yaml:"run"`
	If              string            `yaml:"if"`
	ContinueOnError any               `yaml:"continue-on-error"`
	Env             map[string]string `yaml:"env"`
	With            map[string]any    `yaml:"with"`
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

	producer, ok := workflow.Jobs["service-artifacts"]
	require.True(t, ok, "CI must build the Compose service artifacts once")
	assertServiceArtifactProducer(t, producer)

	for _, consumer := range []struct {
		job    string
		target string
	}{
		{"bvt", "cd tests && make test-bvt"},
		{"func", "cd tests && make test-func"},
		{"reliability", "cd tests && make test-reliability"},
		{"perf-cache", "cd tests && make test-perf-cache-gate"},
	} {
		job, exists := workflow.Jobs[consumer.job]
		require.True(t, exists, "%s must be a dedicated clean-runner consumer", consumer.job)
		require.Equal(t, "service-artifacts", job.Needs)
		assertFullStackGateJob(t, job, consumer.target)
		assertInvalidGateJobsRejected(t, job, consumer.target)
	}

	reliability, ok := workflow.Jobs["reliability"]
	require.True(t, ok, "RL-05 must have a dedicated reliability job")
	require.Equal(t, "service-artifacts", reliability.Needs)
	require.Equal(t, "github.event_name == 'pull_request' || github.event_name == 'schedule'", reliability.If)

	perfCache, ok := workflow.Jobs["perf-cache"]
	require.True(t, ok, "PF-09 must have a dedicated perf-cache job")
	require.Equal(t, "service-artifacts", perfCache.Needs)
	require.Equal(t, "github.event_name == 'schedule'", perfCache.If)
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

const (
	artifactName            = "compose-service-artifacts"
	artifactPath            = "compose-artifacts"
	builderImage            = "chatnow-ci-builder:ci"
	consumerValidateCommand = `docker run --rm -v "$PWD:/workspace" -w /workspace ubuntu:24.04@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90 ./scripts/validate_compose_artifacts.sh compose-artifacts`
	nativeBuildCommand      = `docker run --rm -v "$PWD:/workspace" -w /workspace chatnow-ci-builder:ci bash -lc '
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)" --target conversation_server gateway_server identity_server media_server message_server presence_server push_server relationship_server transmite_server
'`
	packageCommand  = `docker run --rm -v "$PWD:/workspace" -w /workspace chatnow-ci-builder:ci ./scripts/package_compose_artifacts.sh build compose-artifacts`
	validateCommand = `docker run --rm -v "$PWD:/workspace" -w /workspace chatnow-ci-builder:ci ./scripts/validate_compose_artifacts.sh compose-artifacts`
	restoreCommand  = `for service in conversation gateway identity media message presence push relationship transmite; do
  rm -rf "$service/build" "$service/depends"
  cp -a "compose-artifacts/$service/build" "$service/build"
  cp -a "compose-artifacts/$service/depends" "$service/depends"
done`
	chmodArtifactsCommand = `for service in conversation gateway identity media message presence push relationship transmite; do
  chmod +x "compose-artifacts/$service/build/${service}_server"
  chmod +x "$service/build/${service}_server"
done`
)

func assertServiceArtifactProducer(t *testing.T, job workflowJob) {
	t.Helper()
	require.NoError(t, validateServiceArtifactProducer(job))

	valid := cloneWorkflowJob(job)
	build := exactUsesStepIndex(valid, "docker/build-push-action@v6")
	native := exactRunStepIndex(valid, nativeBuildCommand)
	pack := exactRunStepIndex(valid, packageCommand)
	validate := exactRunStepIndex(valid, validateCommand)
	upload := exactUsesStepIndex(valid, "actions/upload-artifact@v4")
	for name, mutate := range map[string]func(*workflowJob){
		"builder allowed to fail":      func(job *workflowJob) { job.Steps[build].ContinueOnError = true },
		"native build allowed to fail": func(job *workflowJob) { job.Steps[native].ContinueOnError = true },
		"package allowed to fail":      func(job *workflowJob) { job.Steps[pack].ContinueOnError = true },
		"validation allowed to fail":   func(job *workflowJob) { job.Steps[validate].ContinueOnError = true },
		"upload allowed to fail":       func(job *workflowJob) { job.Steps[upload].ContinueOnError = true },
		"validation after upload": func(job *workflowJob) {
			job.Steps[validate], job.Steps[upload] = job.Steps[upload], job.Steps[validate]
		},
		"native build bypassed":   func(job *workflowJob) { job.Steps[native].If = "${{ false }}" },
		"native build duplicated": func(job *workflowJob) { job.Steps = append(job.Steps, job.Steps[native]) },
	} {
		t.Run("producer rejects "+name, func(t *testing.T) {
			invalid := cloneWorkflowJob(valid)
			mutate(&invalid)
			require.Error(t, validateServiceArtifactProducer(invalid))
		})
	}
}

func validateServiceArtifactProducer(job workflowJob) error {
	if countExactUsesSteps(job, "docker/build-push-action@v6") != 1 || countExactRunSteps(job, nativeBuildCommand) != 1 {
		return fmt.Errorf("services must be built exactly once")
	}
	ordered := []struct {
		label string
		index int
	}{
		{"checkout", exactUsesStepIndex(job, "actions/checkout@v4")},
		{"Buildx setup", exactUsesStepIndex(job, "docker/setup-buildx-action@v3")},
		{"builder image build", exactUsesStepIndex(job, "docker/build-push-action@v6")},
		{"native service build", exactRunStepIndex(job, nativeBuildCommand)},
		{"artifact package", exactRunStepIndex(job, packageCommand)},
		{"artifact validation", exactRunStepIndex(job, validateCommand)},
		{"artifact upload", exactUsesStepIndex(job, "actions/upload-artifact@v4")},
	}
	previous := -1
	for _, required := range ordered {
		if required.index < 0 || required.index <= previous {
			return fmt.Errorf("missing or out-of-order %s step", required.label)
		}
		step := job.Steps[required.index]
		if strings.TrimSpace(step.If) != "" || continueOnErrorEnabled(step.ContinueOnError) {
			return fmt.Errorf("%s step must fail closed", required.label)
		}
		previous = required.index
	}
	build := job.Steps[ordered[2].index]
	if build.With["context"] != "." || build.With["file"] != "docker/ci/Dockerfile" || build.With["load"] != true || build.With["tags"] != builderImage || build.With["cache-from"] != "type=gha" || build.With["cache-to"] != "type=gha,mode=max" {
		return fmt.Errorf("builder image must use the Dockerfile, local load, and BuildKit GHA cache")
	}
	upload := job.Steps[ordered[len(ordered)-1].index]
	if upload.With["name"] != artifactName || upload.With["path"] != artifactPath || upload.With["if-no-files-found"] != "error" {
		return fmt.Errorf("artifact upload contract is incomplete")
	}
	return nil
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
		if strings.TrimSpace(step.Run) == strings.TrimSpace(wanted) {
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

func countExactUsesSteps(job workflowJob, wanted string) int {
	count := 0
	for _, step := range job.Steps {
		if step.Uses == wanted {
			count++
		}
	}
	return count
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
	setupGo := exactUsesStepIndex(valid, "actions/setup-go@v5")
	download := exactUsesStepIndex(valid, "actions/download-artifact@v4")
	restore := exactRunStepIndex(valid, restoreCommand)
	chmod := exactRunStepIndex(valid, chmodArtifactsCommand)
	validate := exactRunStepIndex(valid, consumerValidateCommand)
	start := exactRunStepIndex(valid, "docker compose up -d --build")
	wait := exactRunStepIndex(valid, "./scripts/wait_for_services.sh")
	proto := exactRunStepIndex(valid, "cd tests && make proto")
	deps := exactRunStepIndex(valid, "cd tests && go mod download")
	teardown := exactRunStepIndex(valid, "docker compose down -v")
	require.NotEqual(t, -1, gate)
	require.NotEqual(t, -1, setupGo)
	require.NotEqual(t, -1, download)
	require.NotEqual(t, -1, restore)
	require.NotEqual(t, -1, chmod)
	require.NotEqual(t, -1, validate)
	require.NotEqual(t, -1, start)
	require.NotEqual(t, -1, wait)
	require.NotEqual(t, -1, proto)
	require.NotEqual(t, -1, deps)
	require.NotEqual(t, -1, teardown)

	for name, mutate := range map[string]func(*workflowJob){
		"Go setup allowed to fail": func(job *workflowJob) {
			job.Steps[setupGo].ContinueOnError = true
		},
		"download allowed to fail": func(job *workflowJob) {
			job.Steps[download].ContinueOnError = true
		},
		"restore allowed to fail": func(job *workflowJob) {
			job.Steps[restore].ContinueOnError = true
		},
		"missing executable mode repair": func(job *workflowJob) {
			job.Steps = append(job.Steps[:chmod], job.Steps[chmod+1:]...)
		},
		"executable mode repair after validation": func(job *workflowJob) {
			job.Steps[chmod], job.Steps[validate] = job.Steps[validate], job.Steps[chmod]
		},
		"artifact validation allowed to fail": func(job *workflowJob) {
			job.Steps[validate].ContinueOnError = true
		},
		"artifact validation on consumer host": func(job *workflowJob) {
			job.Steps[validate].Run = "./scripts/validate_compose_artifacts.sh compose-artifacts"
		},
		"artifact validation with mutable Ubuntu tag": func(job *workflowJob) {
			job.Steps[validate].Run = `docker run --rm -v "$PWD:/workspace" -w /workspace ubuntu:24.04 ./scripts/validate_compose_artifacts.sh compose-artifacts`
		},
		"gate disabled by if": func(job *workflowJob) {
			job.Steps[gate].If = "${{ false }}"
		},
		"gate allowed to fail": func(job *workflowJob) {
			job.Steps[gate].ContinueOnError = true
		},
		"startup allowed to fail": func(job *workflowJob) {
			job.Steps[start].ContinueOnError = true
		},
		"wait allowed to fail": func(job *workflowJob) {
			job.Steps[wait].ContinueOnError = true
		},
		"proto allowed to fail": func(job *workflowJob) {
			job.Steps[proto].ContinueOnError = true
		},
		"dependency download allowed to fail": func(job *workflowJob) {
			job.Steps[deps].ContinueOnError = true
		},
		"exit zero after gate": func(job *workflowJob) {
			job.Steps[gate].Run = target + "\nexit 0"
		},
		"if false gate": func(job *workflowJob) {
			job.Steps[gate].Run = "if false; then " + target + "; fi"
		},
		"quoted skip target": func(job *workflowJob) {
			insertWorkflowStep(job, teardown, workflowStep{Run: `cd tests && make "test-perf-cache"`})
		},
		"single-quoted skip target": func(job *workflowJob) {
			insertWorkflowStep(job, teardown, workflowStep{Run: `cd tests && make 'test-perf-cache'`})
		},
		"gate before dependencies": func(job *workflowJob) {
			job.Steps[gate], job.Steps[deps] = job.Steps[deps], job.Steps[gate]
		},
		"Compose before artifact validation": func(job *workflowJob) {
			job.Steps[start], job.Steps[validate] = job.Steps[validate], job.Steps[start]
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

func insertWorkflowStep(job *workflowJob, index int, step workflowStep) {
	job.Steps = append(job.Steps, workflowStep{})
	copy(job.Steps[index+1:], job.Steps[index:])
	job.Steps[index] = step
}

func cloneWorkflowJob(job workflowJob) workflowJob {
	clone := job
	clone.Steps = append([]workflowStep(nil), job.Steps...)
	return clone
}

func assertTargetAbsent(t *testing.T, job workflowJob, target string) {
	t.Helper()
	for _, step := range job.Steps {
		require.False(t, isForbiddenMakeTarget(step.Run, target),
			"PF-09 CI must not execute the skip-capable discovery target")
	}
}

func isForbiddenMakeTarget(run, target string) bool {
	run = strings.TrimSpace(run)
	for _, command := range []string{
		"cd tests && make " + target,
		`cd tests && make "` + target + `"`,
		"cd tests && make '" + target + "'",
	} {
		if run == command {
			return true
		}
	}
	return false
}

func validateFullStackGateJob(job workflowJob, target string) error {
	const install = "sudo apt-get update\nsudo apt-get install -y protobuf-compiler netcat-openbsd"
	if job.Needs != "service-artifacts" {
		return fmt.Errorf("gate job must depend on service-artifacts")
	}
	for _, step := range job.Steps {
		if isForbiddenMakeTarget(step.Run, "test-perf-cache") {
			return fmt.Errorf("skip-capable performance target is forbidden")
		}
	}
	ordered := []struct {
		label string
		index int
	}{
		{"checkout", exactUsesStepIndex(job, "actions/checkout@v4")},
		{"Go setup", exactUsesStepIndex(job, "actions/setup-go@v5")},
		{"system dependency install", exactRunStepIndex(job, install)},
		{"protoc generator install", exactRunStepIndex(job, "go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11")},
		{"artifact download", exactUsesStepIndex(job, "actions/download-artifact@v4")},
		{"artifact restore", exactRunStepIndex(job, restoreCommand)},
		{"executable mode repair", exactRunStepIndex(job, chmodArtifactsCommand)},
		{"artifact validation", exactRunStepIndex(job, consumerValidateCommand)},
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
	gate := ordered[len(ordered)-2].index
	if strings.TrimSpace(job.Steps[gate].If) != "" {
		return fmt.Errorf("gate step must not have a step-level if condition")
	}
	for _, required := range ordered[:len(ordered)-1] {
		if continueOnErrorEnabled(job.Steps[required.index].ContinueOnError) {
			return fmt.Errorf("%s step must not continue on error", required.label)
		}
		if strings.TrimSpace(job.Steps[required.index].If) != "" {
			return fmt.Errorf("%s step must not have a step-level if condition", required.label)
		}
	}
	download := ordered[4].index
	if job.Steps[download].With["name"] != artifactName || job.Steps[download].With["path"] != artifactPath {
		return fmt.Errorf("gate job must download the shared Compose artifact")
	}
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

func continueOnErrorEnabled(value any) bool {
	switch value := value.(type) {
	case nil:
		return false
	case bool:
		return value
	default:
		return true
	}
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
