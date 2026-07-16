package contracts

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
)

var composeArtifactServices = []string{
	"conversation", "gateway", "identity", "media", "message",
	"presence", "push", "relationship", "transmite",
}

func TestComposeArtifactBuilderIsLocked(t *testing.T) {
	root := repositoryRoot(t)
	dockerfile := readContractFile(t, root, "docker/ci/Dockerfile")
	lockfile := readContractFile(t, root, "docker/ci/dependencies.lock")

	imageLock := regexp.MustCompile(`(?m)^UBUNTU_IMAGE=(ubuntu:24\.04@sha256:[0-9a-f]{64})$`).FindStringSubmatch(lockfile)
	require.Len(t, imageLock, 2, "the Ubuntu builder image must be locked by tag and digest")
	require.Contains(t, dockerfile, "ARG UBUNTU_IMAGE="+imageLock[1])
	require.Contains(t, dockerfile, "FROM ${UBUNTU_IMAGE}")
	require.Contains(t, dockerfile, "COPY docker/ci/dependencies.lock")
	require.Contains(t, dockerfile, ". /tmp/dependencies.lock")
	require.NotContains(t, strings.ToLower(dockerfile), ":latest")
	require.NotRegexp(t, regexp.MustCompile(`(?m)git (clone|checkout).*(main|master)(\s|$)`), dockerfile)

	for _, dependency := range []string{
		"BRPC_REV", "ETCD_CPP_APIV3_REV", "REDIS_PLUS_PLUS_REV", "CPR_REV",
		"ELASTICLIENT_REV", "AMQP_CPP_REV", "AWS_SDK_CPP_REV",
	} {
		revision := regexp.MustCompile(`(?m)^` + dependency + `=([0-9a-f]{40})$`).FindStringSubmatch(lockfile)
		require.Len(t, revision, 2, "%s must be an immutable 40-character Git revision", dependency)
		require.Contains(t, dockerfile, `"$`+dependency+`"`, "%s must be consumed by the builder", dependency)
	}
	require.Contains(t, dockerfile, "ldconfig")
}

func TestComposeArtifactPackagerContract(t *testing.T) {
	root := repositoryRoot(t)
	scriptPath := filepath.Join(root, "scripts/package_compose_artifacts.sh")
	script := readContractFile(t, root, "scripts/package_compose_artifacts.sh")

	require.Contains(t, script, "set -euo pipefail")
	assertExplicitArtifactServices(t, script)
	require.Contains(t, script, `build_root="${1:-build}"`)
	require.Contains(t, script, `artifact_root="${2:-compose-artifacts}"`)
	require.Contains(t, script, `"$build_root/$service/${service}_server"`)
	require.Contains(t, script, `[[ -x "$binary" ]]`)
	require.Contains(t, script, "not found")
	require.Contains(t, script, "sha256sum")
	require.Contains(t, script, "sort -z")

	t.Run("rejects missing service binary", func(t *testing.T) {
		buildRoot := filepath.Join(t.TempDir(), "build")
		result := exec.Command("bash", scriptPath, buildRoot, filepath.Join(t.TempDir(), "artifacts"))
		output, err := result.CombinedOutput()
		require.Error(t, err)
		require.Contains(t, string(output), "missing executable service binary")
	})

	t.Run("rejects unresolved ldd dependency", func(t *testing.T) {
		temp := t.TempDir()
		buildRoot := filepath.Join(temp, "build")
		for _, service := range composeArtifactServices {
			binary := filepath.Join(buildRoot, service, service+"_server")
			require.NoError(t, os.MkdirAll(filepath.Dir(binary), 0o755))
			require.NoError(t, os.WriteFile(binary, []byte("#!/bin/sh\nexit 0\n"), 0o755))
		}
		binDir := filepath.Join(temp, "bin")
		require.NoError(t, os.MkdirAll(binDir, 0o755))
		ldd := filepath.Join(binDir, "ldd")
		require.NoError(t, os.WriteFile(ldd, []byte("#!/bin/sh\necho 'libmissing.so => not found'\n"), 0o755))

		result := exec.Command("bash", scriptPath, buildRoot, filepath.Join(temp, "artifacts"))
		result.Env = append(os.Environ(), "PATH="+binDir+":"+os.Getenv("PATH"))
		output, err := result.CombinedOutput()
		require.Error(t, err)
		require.Contains(t, string(output), "unresolved shared library")
	})
}

func TestComposeArtifactValidatorContract(t *testing.T) {
	root := repositoryRoot(t)
	script := readContractFile(t, root, "scripts/validate_compose_artifacts.sh")

	require.Contains(t, script, "set -euo pipefail")
	assertExplicitArtifactServices(t, script)
	require.Contains(t, script, `artifact_root="${1:-compose-artifacts}"`)
	require.Contains(t, script, "sha256sum --check")
	require.Contains(t, script, `[[ -x "$binary" ]]`)
	require.Contains(t, script, "env -i")
	require.Contains(t, script, `LD_LIBRARY_PATH="$depends_dir"`)
	require.Contains(t, script, "not found")
	require.Contains(t, script, `"$depends_dir/$library_name"`)
}

func assertExplicitArtifactServices(t *testing.T, script string) {
	t.Helper()
	serviceList := strings.Join(composeArtifactServices, " ")
	require.Contains(t, script, "services=("+serviceList+")",
		"the nine Compose services must be enumerated explicitly and in a stable order")
}

func readContractFile(t *testing.T, root, name string) string {
	t.Helper()
	contents, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(name)))
	require.NoError(t, err, "%s must exist", name)
	return string(contents)
}
