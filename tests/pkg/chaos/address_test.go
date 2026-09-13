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
  "network connect"|"create --network")
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
		"CHATNOW_ADDRESS_CALLS="+log, "PATH="+dir+string(os.PathListSeparator)+os.Getenv("PATH"))
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
}
