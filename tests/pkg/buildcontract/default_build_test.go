package buildcontract

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"testing"
	"time"
)

// BLD-01 | P0 | The configured native build graph contains exactly the nine services.
func TestDefaultBuildContainsOnlyServices(t *testing.T) {
	source := os.Getenv("CHATNOW_CMAKE_SOURCE")
	if source == "" {
		t.Skip("set CHATNOW_CMAKE_SOURCE to a checkout with native build dependencies")
	}
	build := t.TempDir()
	query := filepath.Join(build, ".cmake", "api", "v1", "query")
	if err := os.MkdirAll(query, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(query, "codemodel-v2"), nil, 0644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("configure native graph: %v\n%s", err, out)
	}
	files, err := filepath.Glob(filepath.Join(build, ".cmake", "api", "v1", "reply", "codemodel-v2-*.json"))
	if err != nil || len(files) != 1 {
		t.Fatalf("expected one CMake codemodel, found %v: %v", files, err)
	}
	raw, err := os.ReadFile(files[0])
	if err != nil {
		t.Fatal(err)
	}
	var model struct {
		Configurations []struct{ Targets []struct{ Name string } }
	}
	if err := json.Unmarshal(raw, &model); err != nil {
		t.Fatal(err)
	}
	if len(model.Configurations) != 1 {
		t.Fatalf("expected one Release configuration, got %d", len(model.Configurations))
	}
	var got []string
	for _, target := range model.Configurations[0].Targets {
		got = append(got, target.Name)
	}
	sort.Strings(got)
	want := []string{"conversation_server", "gateway_server", "identity_server", "media_server", "message_server", "presence_server", "push_server", "relationship_server", "transmite_server"}
	if !reflect.DeepEqual(want, got) {
		t.Fatalf("default graph must contain only service targets\nwant: %v\ngot:  %v", want, got)
	}
	t.Logf("configured Release targets: %v", got)
}
