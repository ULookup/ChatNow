package agentpolicy

import (
	"regexp"
	"strings"
)

var conventionalCommitPattern = regexp.MustCompile(`^(?:feat|fix|refactor|test|docs|chore|build|ci|perf|revert)(?:\([a-z0-9]+(?:-[a-z0-9]+)*\))?!?: [a-z][\x20-\x7e]*\z`)

// ValidateCommitSubjects validates English Conventional Commit subjects.
func ValidateCommitSubjects(subjects []string) []Violation {
	var violations []Violation
	for _, subject := range subjects {
		if !conventionalCommitPattern.MatchString(subject) || strings.HasSuffix(subject, ".") {
			violations = append(violations, Violation{
				Rule:    "COMMIT_SUBJECT_INVALID",
				Message: "Commit subject must be an English Conventional Commit without a trailing period",
			})
		}
	}
	return violations
}
