package chaos

import (
	"context"
	"encoding/json"
	"fmt"
	"net/netip"
	"os/exec"
	"slices"
	"strings"
	"testing"
	"time"

	"chatnow-tests/pkg/client"
)

type containerAddress struct {
	ID, StartedAt, Project, Image string
	Running                       bool
	Networks                      map[string]struct {
		IPAddress, NetworkID string
		Aliases              []string
	}
}

func dockerAddress(cfg *client.Config, args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "docker", args...)
	cmd.Dir = cfg.Infra.ComposeDir
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("docker %v: %w: %s", args, err, out)
	}
	return out, nil
}

func inspectAddress(cfg *client.Config, id string) (containerAddress, error) {
	// Select only network/process metadata; never collect container credentials.
	format := `{"ID":{{json .Id}},"Image":{{json .Config.Image}},"StartedAt":{{json .State.StartedAt}},"Running":{{json .State.Running}},"Project":{{json (index .Config.Labels "com.docker.compose.project")}},"Networks":{{json .NetworkSettings.Networks}}}`
	out, err := dockerAddress(cfg, "inspect", "--format", format, id)
	var result containerAddress
	if err == nil {
		err = json.Unmarshal(out, &result)
	}
	return result, err
}

// ChangeIdentityAddress changes only the selected Compose project's Identity
// endpoint and restarts Identity to reconnect its outgoing clients at the new IP.
// Cleanup restores its original address and aliases, including on RED.
// The returned check proves Gateway and Transmite were not restarted to recover.
func ChangeIdentityAddress(t testing.TB, cfg *client.Config) func() {
	t.Helper()
	services := []string{"identity_server", "gateway_server", "transmite_server"}
	before := make([]containerAddress, len(services))
	for i, service := range services {
		out, err := dockerAddress(cfg, "compose", "ps", "-q", service)
		if err != nil || len(strings.Fields(string(out))) != 1 {
			t.Fatalf("expected one running Compose %s container: %v", service, err)
		}
		before[i], err = inspectAddress(cfg, strings.TrimSpace(string(out)))
		if err != nil || !before[i].Running || before[i].Project == "" {
			t.Fatalf("inspect running Compose %s: %v", service, err)
		}
		if before[i].Project != before[0].Project {
			t.Fatal("fault endpoints must belong to one Compose project")
		}
	}
	identity := before[0]
	if len(identity.Networks) != 1 {
		t.Fatal("address fault requires exactly one Identity network")
	}
	var network, networkID, original string
	var aliases []string
	for name, endpoint := range identity.Networks {
		network, networkID, original, aliases = name, endpoint.NetworkID, endpoint.IPAddress, endpoint.Aliases
	}
	var networks []struct {
		Labels map[string]string
		IPAM   struct {
			Config []struct{ Subnet, Gateway string }
		}
		Containers map[string]struct{ IPv4Address string }
	}
	out, err := dockerAddress(cfg, "network", "inspect", networkID)
	if err != nil || json.Unmarshal(out, &networks) != nil || len(networks) != 1 {
		t.Fatalf("inspect fault network: %v", err)
	}
	n := networks[0]
	if n.Labels["com.docker.compose.project"] != identity.Project {
		t.Fatal("refusing to change an external/shared network")
	}
	oldIP, err := netip.ParseAddr(original)
	if err != nil || !oldIP.Is4() {
		t.Fatal("address fault requires a Compose IPv4 endpoint")
	}
	used := map[netip.Addr]bool{oldIP: true}
	var subnet netip.Prefix
	for _, c := range n.IPAM.Config {
		p, e := netip.ParsePrefix(c.Subnet)
		if e == nil && p.Contains(oldIP) {
			subnet = p.Masked()
		}
		if gateway, e := netip.ParseAddr(c.Gateway); e == nil {
			used[gateway] = true
		}
	}
	for _, c := range n.Containers {
		if p, e := netip.ParsePrefix(c.IPv4Address); e == nil {
			used[p.Addr()] = true
		}
	}
	if !subnet.IsValid() || !subnet.Addr().Is4() {
		t.Fatal("no IPv4 subnet covers the isolated Identity endpoint")
	}
	last := subnet.Addr().As4()
	for bit := subnet.Bits(); bit < 32; bit++ {
		last[bit/8] |= 1 << (7 - bit%8)
	}
	// Prefer the high end, away from normal low-address Compose allocation.
	// Network inspection can omit reservations held by stopped containers, so
	// Docker's actual allocation probe remains authoritative.
	candidate := netip.AddrFrom4(last).Prev()
	var replacement netip.Addr
	// Older Docker engines reject explicit IPs on auto-allocated subnets. Prove
	// support with an inert, test-owned endpoint before touching the live service.
	// No credentials or application entrypoint are copied into the probe.
	for attempt := 0; attempt < 16; attempt++ {
		for subnet.Contains(candidate) && candidate != subnet.Addr() && used[candidate] {
			candidate = candidate.Prev()
		}
		if !subnet.Contains(candidate) || candidate == subnet.Addr() {
			break
		}
		used[candidate] = true
		out, probeErr := dockerAddress(cfg, "create", "--network", networkID, "--ip", candidate.String(),
			"--entrypoint", "/bin/true", identity.Image)
		if probeErr == nil {
			probeID := strings.TrimSpace(string(out))
			probeRemoved := false
			t.Cleanup(func() {
				if !probeRemoved {
					if _, e := dockerAddress(cfg, "rm", "--force", probeID); e != nil {
						t.Errorf("remove address capability probe: %v", e)
					}
				}
			})
			_, probeErr = dockerAddress(cfg, "start", "--attach", probeID)
			if _, e := dockerAddress(cfg, "rm", "--force", probeID); e != nil {
				t.Fatalf("release address capability probe: %v", e)
			}
			probeRemoved = true
		}
		if probeErr == nil {
			replacement = candidate
			break
		}
		if !strings.Contains(strings.ToLower(probeErr.Error()), "address already in use") {
			t.Fatalf("address fault needs a user configured subnet (tests/compose/reliability.yml), candidate %s: %v", candidate, probeErr)
		}
		t.Logf("Docker reserves candidate %s; probing another address", candidate)
	}
	if !replacement.IsValid() {
		t.Fatal("no reservable address found within 16 probes of the isolated Compose network")
	}
	connect := func(ip string) error {
		args := []string{"network", "connect", "--ip", ip}
		for _, alias := range aliases {
			args = append(args, "--alias", alias)
		}
		_, e := dockerAddress(cfg, append(args, networkID, identity.ID)...)
		return e
	}
	aliasesMatch := func(got []string) bool {
		want := slices.Clone(aliases)
		got = slices.Clone(got)
		slices.Sort(want)
		slices.Sort(got)
		return slices.Equal(got, want)
	}
	check := func() {
		t.Helper()
		for i, b := range before[1:] {
			after, e := inspectAddress(cfg, b.ID)
			if e != nil || !after.Running || after.StartedAt != b.StartedAt {
				t.Errorf("%s process changed during address recovery: %v", services[i+1], e)
			}
		}
	}
	// Arm restoration before the first mutation, including uncertain CLI exits.
	t.Cleanup(func() {
		after, e := inspectAddress(cfg, identity.ID)
		if e != nil {
			t.Errorf("inspect Identity before network restoration: %v", e)
			return
		}
		if endpoint, connected := after.Networks[network]; connected {
			if endpoint.IPAddress == original && aliasesMatch(endpoint.Aliases) {
				check()
				return
			}
			if _, e = dockerAddress(cfg, "network", "disconnect", networkID, identity.ID); e != nil {
				t.Errorf("disconnect changed Identity address: %v", e)
				return
			}
		}
		if e = connect(original); e != nil {
			t.Errorf("restore original Identity address: %v", e)
			return
		}
		if _, e = dockerAddress(cfg, "restart", "--time", "1", identity.ID); e != nil {
			t.Errorf("restart restored Identity endpoint: %v", e)
		}
		restored, e := inspectAddress(cfg, identity.ID)
		if e != nil || restored.Networks[network].IPAddress != original || !aliasesMatch(restored.Networks[network].Aliases) {
			t.Errorf("original Identity address and aliases were not restored: %v", e)
		}
		t.Log("original Identity address and aliases restored")
		check()
	})
	if _, err = dockerAddress(cfg, "network", "disconnect", networkID, identity.ID); err != nil {
		t.Fatal(err)
	}
	if err = connect(replacement.String()); err != nil {
		t.Fatal(err)
	}
	// Moving an interface breaks existing outbound sockets as well. Restart only
	// the moved service to renew its registry lease and datastore connections;
	// otherwise lease expiration would confound the caller DNS recovery assertion.
	if _, err = dockerAddress(cfg, "restart", "--time", "1", identity.ID); err != nil {
		t.Fatal(err)
	}
	after, err := inspectAddress(cfg, identity.ID)
	if err != nil || after.Networks[network].IPAddress != replacement.String() {
		t.Fatalf("Identity did not acquire the replacement address: %v", err)
	}
	t.Logf("Identity address changed from %s to %s without restarting callers", original, replacement)
	return check
}
