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
	require.Contains(t, dockerfile, "git -C /tmp/aws-sdk-cpp submodule update --init --recursive --depth 1")
	require.NotContains(t, dockerfile, "submodule update --remote")

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
	require.Contains(t, script, "depends_dir_real")
	require.Contains(t, script, "resolved_library")
}

func TestComposeArtifactScriptsEndToEnd(t *testing.T) {
	root := repositoryRoot(t)

	t.Run("packages all services and validates manifest", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)

		manifest := readContractFile(t, fixture.artifactRoot, "MANIFEST.sha256")
		for _, service := range composeArtifactServices {
			require.FileExists(t, filepath.Join(fixture.artifactRoot, service, "build", service+"_server"))
			require.FileExists(t, filepath.Join(fixture.artifactRoot, service, "depends", "libfixture.so"))
			require.Contains(t, manifest, "./"+service+"/build/"+service+"_server")
			require.Contains(t, manifest, "./"+service+"/depends/libfixture.so")
		}

		output, err := fixture.validate(t, fixture.lddPath)
		require.NoError(t, err, "%s", output)
	})

	t.Run("rejects manifest tampering", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)
		tampered := filepath.Join(fixture.artifactRoot, "identity", "build", "identity_server")
		file, err := os.OpenFile(tampered, os.O_APPEND|os.O_WRONLY, 0)
		require.NoError(t, err)
		_, err = file.WriteString("tampered\n")
		require.NoError(t, err)
		require.NoError(t, file.Close())

		output, err := fixture.validate(t, fixture.lddPath)
		require.Error(t, err)
		require.Contains(t, output, "manifest")
	})

	t.Run("rejects missing packaged library", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)
		require.NoError(t, os.Remove(filepath.Join(fixture.artifactRoot, "media", "depends", "libfixture.so")))
		fixture.rewriteManifest(t)

		output, err := fixture.validate(t, fixture.lddPath)
		require.Error(t, err)
		require.Contains(t, output, "outside packaged closure")
	})

	t.Run("rejects host library fallback with matching basename", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)
		hostLibrary := filepath.Join(fixture.temp, "host", "libfixture.so")
		require.NoError(t, os.MkdirAll(filepath.Dir(hostLibrary), 0o755))
		require.NoError(t, os.WriteFile(hostLibrary, []byte("host library\n"), 0o644))
		fallbackLDD := fixture.writeLDD(t, filepath.Join(fixture.tools, "ldd-host"), hostLibrary)

		output, err := fixture.validate(t, fallbackLDD)
		require.Error(t, err)
		require.Contains(t, output, "outside packaged closure")
	})

	t.Run("rejects packaged symlink escaping to host library", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)
		hostLibrary := filepath.Join(fixture.temp, "host", "libfixture.so")
		require.NoError(t, os.MkdirAll(filepath.Dir(hostLibrary), 0o755))
		require.NoError(t, os.WriteFile(hostLibrary, []byte("host library\n"), 0o644))
		packagedLibrary := filepath.Join(fixture.artifactRoot, "push", "depends", "libfixture.so")
		require.NoError(t, os.Remove(packagedLibrary))
		require.NoError(t, os.Symlink(hostLibrary, packagedLibrary))
		fixture.rewriteManifest(t)

		output, err := fixture.validate(t, fixture.lddPath)
		require.Error(t, err)
		require.Contains(t, output, "outside packaged closure")
	})

	t.Run("rejects unlisted artifact file", func(t *testing.T) {
		fixture := newArtifactFixture(t, root)
		fixture.packageArtifacts(t)
		require.NoError(t, os.WriteFile(filepath.Join(fixture.artifactRoot, "unlisted.txt"), []byte("extra\n"), 0o644))

		output, err := fixture.validate(t, fixture.lddPath)
		require.Error(t, err)
		require.Contains(t, output, "unlisted artifact file")
	})
}

type artifactFixture struct {
	root          string
	temp          string
	buildRoot     string
	artifactRoot  string
	tools         string
	sha256sumPath string
	lddPath       string
}

func newArtifactFixture(t *testing.T, root string) artifactFixture {
	t.Helper()
	temp := t.TempDir()
	fixture := artifactFixture{
		root:         root,
		temp:         temp,
		buildRoot:    filepath.Join(temp, "build"),
		artifactRoot: filepath.Join(temp, "compose-artifacts"),
		tools:        filepath.Join(temp, "tools"),
	}
	require.NoError(t, os.MkdirAll(fixture.tools, 0o755))
	fixture.sha256sumPath = filepath.Join(fixture.tools, "sha256sum")
	require.NoError(t, os.WriteFile(fixture.sha256sumPath, []byte(`#!/bin/sh
set -eu
checksum() { cksum "$1" | awk '{ print $1 ":" $2 }'; }
if [ "${1:-}" = "--check" ]; then
    manifest="$2"
    status=0
    while read -r expected file; do
        file="${file# }"
        actual="$(checksum "$file")"
        if [ "$actual" != "$expected" ]; then
            echo "$file: FAILED" >&2
            status=1
        fi
    done < "$manifest"
    exit "$status"
fi
for file in "$@"; do
    printf '%s  %s\n' "$(checksum "$file")" "$file"
done
`), 0o755))

	fixture.lddPath = fixture.writeLDD(t, filepath.Join(fixture.tools, "ldd"), "")
	for _, service := range composeArtifactServices {
		binary := filepath.Join(fixture.buildRoot, service, service+"_server")
		require.NoError(t, os.MkdirAll(filepath.Dir(binary), 0o755))
		require.NoError(t, os.WriteFile(binary, []byte("#!/bin/sh\nexit 0\n"), 0o755))
	}
	return fixture
}

func (fixture artifactFixture) writeLDD(t *testing.T, path, forcedLibrary string) string {
	t.Helper()
	script := "#!/bin/sh\nset -eu\n"
	if forcedLibrary != "" {
		script += "echo 'libfixture.so => " + forcedLibrary + " (0x1)'\n"
	} else {
		script += `if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    library="$LD_LIBRARY_PATH/libfixture.so"
else
    library="` + filepath.Join(fixture.temp, "libfixture.so") + `"
fi
echo "libfixture.so => $library (0x1)"
`
	}
	require.NoError(t, os.WriteFile(path, []byte(script), 0o755))
	if forcedLibrary == "" {
		require.NoError(t, os.WriteFile(filepath.Join(fixture.temp, "libfixture.so"), []byte("fixture library\n"), 0o644))
	}
	return path
}

func (fixture artifactFixture) packageArtifacts(t *testing.T) {
	t.Helper()
	command := exec.Command("bash", filepath.Join(fixture.root, "scripts/package_compose_artifacts.sh"), fixture.buildRoot, fixture.artifactRoot)
	command.Env = append(os.Environ(), "PATH="+fixture.tools+":"+os.Getenv("PATH"))
	output, err := command.CombinedOutput()
	require.NoError(t, err, "%s", output)
}

func (fixture artifactFixture) validate(t *testing.T, lddPath string) (string, error) {
	t.Helper()
	command := exec.Command("bash", filepath.Join(fixture.root, "scripts/validate_compose_artifacts.sh"), fixture.artifactRoot)
	command.Env = append(os.Environ(), "LDD="+lddPath, "SHA256SUM="+fixture.sha256sumPath)
	output, err := command.CombinedOutput()
	return string(output), err
}

func (fixture artifactFixture) rewriteManifest(t *testing.T) {
	t.Helper()
	command := exec.Command("bash", "-c", `find . \( -type f -o -type l \) ! -name MANIFEST.sha256 -print0 | LC_ALL=C sort -z | xargs -0 "$SHA256SUM" > MANIFEST.sha256`)
	command.Dir = fixture.artifactRoot
	command.Env = append(os.Environ(), "SHA256SUM="+fixture.sha256sumPath)
	output, err := command.CombinedOutput()
	require.NoError(t, err, "%s", output)
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
