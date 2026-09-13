package chaos

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"chatnow-tests/pkg/client"
)

func TestAddressFaultRejectsUnsupportedNetworkBeforeDisconnect(t *testing.T) {
	if os.Getenv("CHATNOW_ADDRESS_PROBE_CHILD") == "1" {
		ChangeIdentityAddress(t, &client.Config{Infra: client.InfraConfig{ComposeDir: os.Getenv("CHATNOW_ADDRESS_PROBE_DIR")}})
		return
	}
	if runtime.GOOS == "windows" {
		t.Skip("Docker command-boundary fixture requires the supported Linux test host")
	}
	for _, mode := range []string{"create", "start", "reserved"} {
		t.Run(mode, func(t *testing.T) {
			checkUnsupportedAddressNetwork(t, mode)
		})
	}
}

func checkUnsupportedAddressNetwork(t *testing.T, mode string) {
	t.Helper()
	dir := t.TempDir()
	// The real fault controller executes this process boundary. It represents an
	// engine that rejects explicit addresses on an automatically allocated subnet.
	script := `#!/bin/sh
printf '%s\n' "$*" >> "$CHATNOW_ADDRESS_CALLS"
case "$1 $2" in
  "compose ps") echo "$4" ;;
  "inspect --format")
    printf '{"ID":"%s","Image":"fixture-image","StartedAt":"same","Running":true,"Project":"fixture","Networks":{"fixture_default":{"IPAddress":"172.30.97.20","NetworkID":"fixture-network","Aliases":["identity_server"]}}}\n' "$4" ;;
  "network inspect")
    echo '[{"Labels":{"com.docker.compose.project":"fixture"},"IPAM":{"Config":[{"Subnet":"172.30.97.0/24","Gateway":"172.30.97.1"}]},"Containers":{}}]' ;;
  "network disconnect") exit 0 ;;
  "create --network")
    if [ "$CHATNOW_ADDRESS_PROBE_MODE" != create ]; then
      echo fixture-probe; exit 0
    fi
    echo 'user specified IP address requires a user configured subnet' >&2
    exit 1 ;;
  "start --attach")
    if [ "$CHATNOW_ADDRESS_PROBE_MODE" = reserved ] && [ "$(grep -c '^start ' "$CHATNOW_ADDRESS_CALLS")" = 1 ]; then
      echo 'failed to set up container networking: Address already in use' >&2
      exit 1
    fi
    echo 'user specified IP address requires a user configured subnet' >&2
    exit 1 ;;
  "rm --force") exit 0 ;;
  "network connect")
    echo 'user specified IP address requires a user configured subnet' >&2
    exit 1 ;;
  *) echo 'unexpected Docker command' >&2; exit 2 ;;
esac
`
	if err := os.WriteFile(filepath.Join(dir, "docker"), []byte(script), 0755); err != nil {
		t.Fatal(err)
	}
	log := filepath.Join(dir, "calls.log")
	cmd := exec.Command(os.Args[0], "-test.run=^TestAddressFaultRejectsUnsupportedNetworkBeforeDisconnect$", "-test.v")
	cmd.Env = append(os.Environ(), "CHATNOW_ADDRESS_PROBE_CHILD=1", "CHATNOW_ADDRESS_PROBE_DIR="+dir,
		"CHATNOW_ADDRESS_PROBE_MODE="+mode, "CHATNOW_ADDRESS_CALLS="+log, "PATH="+dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	out, err := cmd.CombinedOutput()
	if err == nil {
		t.Fatal("unsupported network must be rejected")
	}
	if !strings.Contains(string(out), "user configured subnet") {
		t.Fatalf("child did not reach the intended network rejection: %s", out)
	}
	calls, err := os.ReadFile(log)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(calls), "network disconnect") {
		t.Fatalf("unsupported network must be rejected before disconnecting Identity; calls:\n%s", calls)
	}
	if mode == "reserved" {
		candidates := make(map[string]bool)
		for _, line := range strings.Split(string(calls), "\n") {
			args := strings.Fields(line)
			if len(args) > 4 && args[0] == "create" {
				candidates[args[4]] = true
			}
		}
		if len(candidates) < 2 || strings.Count(string(calls), "rm --force") < 2 {
			t.Fatalf("a reserved candidate must be released and replaced before rejecting the network; calls:\n%s", calls)
		}
	}
}
