package agentpolicy

import (
	"regexp"
	"strconv"
)

var (
	taskBranchPattern  = regexp.MustCompile(`^(feat|fix|refactor|test|docs|chore)/([1-9][0-9]*)-[a-z0-9]+(?:-[a-z0-9]+)*$`)
	versionBasePattern = regexp.MustCompile(`^[1-9][0-9]*\.(?:0|[1-9][0-9]*)-dev$`)
)

// ValidateBranch validates a task branch against its primary Issue and target
// version development line.
func ValidateBranch(head, base string, issueNumber int) []Violation {
	var violations []Violation
	matches := taskBranchPattern.FindStringSubmatch(head)
	if matches == nil {
		violations = append(violations, Violation{
			Rule:    "BRANCH_FORMAT_INVALID",
			Message: "Task branch must use <type>/<issue>-<slug>",
		})
	}

	if issueNumber <= 0 {
		violations = append(violations, Violation{
			Rule:    "BRANCH_ISSUE_REQUIRED",
			Message: "Task branch requires a primary Issue number",
		})
	} else if matches != nil {
		branchIssue, _ := strconv.Atoi(matches[2])
		if branchIssue != issueNumber {
			violations = append(violations, Violation{
				Rule:    "BRANCH_ISSUE_MISMATCH",
				Message: "Task branch Issue number must match the primary Issue",
			})
		}
	}

	if !versionBasePattern.MatchString(base) {
		violations = append(violations, Violation{
			Rule:    "BRANCH_BASE_INVALID",
			Message: "Task pull request base must be a version development branch",
		})
	}
	return violations
}
