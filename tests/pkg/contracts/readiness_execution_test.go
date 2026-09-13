package contracts

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// Repository contract | P0 | Readiness never puts MinIO credentials in process arguments.
func TestReadinessMinIOCredentialsStayOffCommandLine(t *testing.T) {
	directory := t.TempDir()
	events := filepath.Join(directory, "events")
	environment := filepath.Join(directory, "probe-environment.sh")
	// Intercept external commands to examine the real readiness script's process
	// boundary. No service, credential store, or network connection is accessed.
	const commands = `
curl() { printf '{"status":"green"}\n'; }
nc() { return 0; }
mc() {
  for argument in "$@"; do
    if [[ "$argument" == "$MINIO_ROOT_USER" || "$argument" == "$MINIO_ROOT_PASSWORD" ]]; then
      printf 'credential argument detected\n' >> "$READINESS_TEST_EVENTS"
      return 47
    fi
  done
  if [[ "$1 $2" == 'alias set' ]]; then
    local user password
    IFS= read -r user
    IFS= read -r password
    [[ "$user" == "$MINIO_ROOT_USER" && "$password" == "$MINIO_ROOT_PASSWORD" ]] || return 48
    printf 'credentials received on stdin\n' >> "$READINESS_TEST_EVENTS"
  elif [[ "$1" == stat ]]; then
    printf 'bucket checked\n' >> "$READINESS_TEST_EVENTS"
  fi
}
docker() {
  case "$*" in
    *'redis-cli -p 6379 cluster info'*)
      printf 'cluster_state:ok\ncluster_slots_assigned:16384\ncluster_slots_ok:16384\ncluster_known_nodes:6\n' ;;
    *information_schema.tables*) printf '17\n' ;;
    *'FROM mysql.user'*) printf '5\n' ;;
    *'list_users'*) printf 'chatnow_transmite\t[]\nchatnow_message\t[]\nchatnow_push\t[]\n' ;;
    *'list_user_permissions'*) printf '/\tpermissions\n' ;;
    *'rabbitmq-diagnostics'*) return 0 ;;
    *'--entrypoint /bin/sh minio-init -ec'*) bash -ec "${!#}" ;;
    *'etcdctl'*)
      for service in identity media transmite message relationship conversation presence push; do
        printf '/service/%s_service/instance\n' "$service"
      done ;;
    *) printf 'unexpected Docker invocation\n' >&2; return 49 ;;
  esac
}
`
	require.NoError(t, os.WriteFile(environment, []byte(commands), 0600))
	writeDockerBoundary(t, directory)
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, "bash", filepath.ToSlash(filepath.Join(repositoryRoot(t), "scripts/wait_for_services.sh")))
	command.Env = append(os.Environ(),
		"PATH="+directory+string(os.PathListSeparator)+os.Getenv("PATH"),
		"BASH_ENV="+filepath.ToSlash(environment),
		"READINESS_TEST_EVENTS="+filepath.ToSlash(events),
		"MINIO_ROOT_USER=synthetic-readiness-user",
		"MINIO_ROOT_PASSWORD=synthetic-readiness-password",
		"CHATNOW_READY_TIMEOUT_SEC=10",
		"CHATNOW_READY_POLL_INTERVAL_SEC=0.1",
	)
	output, runError := command.CombinedOutput()
	observations, err := os.ReadFile(events)
	require.NoError(t, err)
	require.NotContains(t, string(observations), "credential argument detected")
	require.NoError(t, runError, "readiness must complete without credentials in argv: %s", output)
	require.Equal(t, 1, strings.Count(string(observations), "credentials received on stdin"))
	require.Equal(t, 2, strings.Count(string(observations), "bucket checked"))
}

// Repository contract | P0 | A stalled Docker probe cannot exceed the readiness deadline.
func TestReadinessDeadlineBoundsStalledProbe(t *testing.T) {
	directory := t.TempDir()
	environment := filepath.Join(directory, "probe-environment.sh")
	require.NoError(t, os.WriteFile(environment, []byte("docker() { sleep 4; return 1; }\n"), 0600))
	writeDockerBoundary(t, directory)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, "bash", filepath.ToSlash(filepath.Join(repositoryRoot(t), "scripts/wait_for_services.sh")))
	command.Env = append(os.Environ(),
		"PATH="+directory+string(os.PathListSeparator)+os.Getenv("PATH"),
		"BASH_ENV="+filepath.ToSlash(environment),
		"CHATNOW_READY_TIMEOUT_SEC=1",
		"CHATNOW_READY_POLL_INTERVAL_SEC=0.1",
	)
	started := time.Now()
	output, err := command.CombinedOutput()
	require.Error(t, err, "a stalled dependency must fail readiness")
	require.Less(t, time.Since(started), 3*time.Second, "deadline did not interrupt the stalled probe")
	require.Contains(t, string(output), "timed out waiting for Redis Cluster")
}

func writeDockerBoundary(t testing.TB, directory string) {
	t.Helper()
	// GNU timeout executes an external command. Its Bash child imports only the
	// test's controlled BASH_ENV and dispatches to the same Docker boundary.
	require.NoError(t, os.WriteFile(filepath.Join(directory, "docker"),
		[]byte("#!/usr/bin/env bash\ndocker \"$@\"\n"), 0700))
}
