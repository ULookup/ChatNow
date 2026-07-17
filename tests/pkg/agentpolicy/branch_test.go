package agentpolicy

import "testing"

func TestValidateBranch(t *testing.T) {
	tests := []struct {
		name        string
		head        string
		base        string
		issueNumber int
		wantRules   []string
	}{
		{name: "valid minor version task", head: "fix/812-cache-resilience", base: "3.1-dev", issueNumber: 812},
		{name: "valid major version task", head: "feat/55-agent-policy", base: "3.0-dev", issueNumber: 55},
		{name: "missing Issue", head: "fix/812-cache-resilience", base: "3.1-dev", wantRules: []string{"BRANCH_ISSUE_REQUIRED"}},
		{name: "mismatched Issue", head: "fix/812-cache-resilience", base: "3.1-dev", issueNumber: 813, wantRules: []string{"BRANCH_ISSUE_MISMATCH"}},
		{name: "task to main", head: "fix/812-cache-resilience", base: "main", issueNumber: 812, wantRules: []string{"BRANCH_BASE_INVALID"}},
		{name: "version branch as task", head: "3.1-dev", base: "3.0-dev", issueNumber: 812, wantRules: []string{"BRANCH_FORMAT_INVALID"}},
		{name: "unsupported prefix", head: "hotfix/812-cache-resilience", base: "3.1-dev", issueNumber: 812, wantRules: []string{"BRANCH_FORMAT_INVALID"}},
		{name: "missing slug", head: "fix/812", base: "3.1-dev", issueNumber: 812, wantRules: []string{"BRANCH_FORMAT_INVALID"}},
		{name: "uppercase slug", head: "fix/812-Cache", base: "3.1-dev", issueNumber: 812, wantRules: []string{"BRANCH_FORMAT_INVALID"}},
		{name: "invalid version base", head: "fix/812-cache-resilience", base: "release/3.1", issueNumber: 812, wantRules: []string{"BRANCH_BASE_INVALID"}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := ValidateBranch(tt.head, tt.base, tt.issueNumber)
			assertExactRules(t, got, tt.wantRules)
		})
	}
}
