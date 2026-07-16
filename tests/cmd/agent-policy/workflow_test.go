package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestAgentPolicyWorkflowContract(t *testing.T) {
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	contentBytes, err := os.ReadFile(filepath.Join(root, ".github", "workflows", "agent-policy.yml"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(contentBytes)

	required := []string{
		"issues:", "opened", "edited", "reopened",
		"pull_request:", "synchronize", "ready_for_review",
		"actions/checkout@v4", "fetch-depth: 0",
		"actions/setup-go@v5", "go-version: '1.24'",
		"github.event.pull_request.base.sha", "github.event.pull_request.head.sha",
		"git diff --name-only", "git log --format=%s",
		"agent-policy\" issue", "agent-policy\" branch", "agent-policy\" commits",
		"agent-policy\" pull-request", "agent-policy\" skill-sync", "agent-policy\" skills",
	}
	for _, value := range required {
		if !strings.Contains(content, value) {
			t.Errorf("agent-policy workflow missing %q", value)
		}
	}
	for _, forbidden := range []string{"pull_request_target", "go run ./cmd/agent-policy"} {
		if strings.Contains(content, forbidden) {
			t.Errorf("agent-policy workflow contains untrusted execution pattern %q", forbidden)
		}
	}

	build := strings.Index(content, "go build -o \"$RUNNER_TEMP/agent-policy\"")
	headCheckout := strings.Index(content, "ref: ${{ github.event.pull_request.head.sha }}")
	if build < 0 || headCheckout < 0 || build >= headCheckout {
		t.Errorf("trusted policy build must occur before head checkout; build=%d head=%d", build, headCheckout)
	}
}
