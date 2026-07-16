package agentpolicy

import "testing"

func TestValidateCommitSubjects(t *testing.T) {
	tests := []struct {
		name      string
		subjects  []string
		wantRules []string
	}{
		{name: "supported subjects", subjects: []string{"feat: add policy", "fix(cache): bound retries", "ci!: enforce policy", "revert: restore queue behavior"}},
		{name: "empty list"},
		{name: "Chinese", subjects: []string{"fix: 修复缓存"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "WIP", subjects: []string{"WIP: cache fix"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "merge", subjects: []string{"Merge branch 'main'"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "invalid type", subjects: []string{"style: format cache"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "uppercase description", subjects: []string{"fix: Bound retries"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "trailing period", subjects: []string{"fix: bound retries."}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "uppercase scope", subjects: []string{"fix(Cache): bound retries"}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "empty description", subjects: []string{"fix: "}, wantRules: []string{"COMMIT_SUBJECT_INVALID"}},
		{name: "one violation per invalid commit", subjects: []string{"fix: valid subject", "WIP", "docs: Bad subject"}, wantRules: []string{"COMMIT_SUBJECT_INVALID", "COMMIT_SUBJECT_INVALID"}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := ValidateCommitSubjects(tt.subjects)
			assertExactRules(t, got, tt.wantRules)
		})
	}
}
