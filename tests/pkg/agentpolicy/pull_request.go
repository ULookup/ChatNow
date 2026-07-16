package agentpolicy

import (
	"regexp"
	"strconv"
	"strings"
)

var (
	pullRequestIssuePattern = regexp.MustCompile(`(?m)^\s*Closes\s+#([1-9][0-9]*)\s*$`)
	pullRequestTitlePattern = regexp.MustCompile(`^(feat|fix|refactor|test|docs|chore)(\([a-z0-9][a-z0-9-]*\))?: [a-z][ -~]*[^.]$`)
)

var pullRequestSectionRules = []issueSectionRule{
	{heading: "Primary Issue", rule: "PR_PRIMARY_ISSUE_REQUIRED"},
	{heading: "Target Version", rule: "PR_TARGET_VERSION_REQUIRED"},
	{heading: "Scope", rule: "PR_SCOPE_REQUIRED"},
	{heading: "Non-goals", rule: "PR_NON_GOALS_REQUIRED"},
	{heading: "Architecture Impact", rule: "PR_ARCHITECTURE_IMPACT_REQUIRED"},
	{heading: "Core-flow Impact", rule: "PR_CORE_FLOW_IMPACT_REQUIRED"},
	{heading: "Updated Skills", rule: "PR_UPDATED_SKILLS_REQUIRED"},
	{heading: "RED Evidence", rule: "PR_RED_EVIDENCE_REQUIRED"},
	{heading: "GREEN Evidence", rule: "PR_GREEN_EVIDENCE_REQUIRED"},
	{heading: "Regression Verification", rule: "PR_REGRESSION_VERIFICATION_REQUIRED"},
	{heading: "Security and Compatibility", rule: "PR_SECURITY_AND_COMPATIBILITY_REQUIRED"},
	{heading: "Unverified Items", rule: "PR_UNVERIFIED_ITEMS_REQUIRED"},
	{heading: "Rollback Plan", rule: "PR_ROLLBACK_PLAN_REQUIRED"},
	{heading: "Stacked PR Dependencies", rule: "PR_STACKED_DEPENDENCIES_REQUIRED"},
	{heading: "Full-diff Self-review", rule: "PR_FULL_DIFF_SELF_REVIEW_REQUIRED"},
}

// ValidatePullRequest validates the machine-checkable ChatNow pull request
// contract. Skill synchronization is deliberately validated separately.
func ValidatePullRequest(input PullRequestInput) []Violation {
	var violations []Violation
	sections := ParseSections(input.Body)

	if !pullRequestTitlePattern.MatchString(strings.TrimSpace(input.Title)) {
		violations = append(violations, Violation{
			Rule:    "PR_TITLE_INVALID",
			Message: "Pull request title must use an allowed English Conventional Commit subject",
		})
	}
	if !containsHanProse(input.Body) {
		violations = append(violations, Violation{
			Rule:    "PR_BODY_CHINESE_REQUIRED",
			Message: "Pull request body prose must contain Chinese text",
		})
	}

	for _, requirement := range pullRequestSectionRules {
		if strings.TrimSpace(sections[requirement.heading]) == "" {
			violations = append(violations, Violation{
				Rule:    requirement.rule,
				Message: "Pull request section " + requirement.heading + " is required",
			})
		}
	}

	issueMatches := pullRequestIssuePattern.FindAllStringSubmatch(sections["Primary Issue"], -1)
	if len(issueMatches) != 1 {
		if !hasRuleID(violations, "PR_PRIMARY_ISSUE_REQUIRED") {
			violations = append(violations, Violation{
				Rule:    "PR_PRIMARY_ISSUE_REQUIRED",
				Message: "Primary Issue must contain exactly one Closes #N declaration",
			})
		}
	} else {
		bodyIssue, _ := strconv.Atoi(issueMatches[0][1])
		if bodyIssue != input.IssueNumber {
			violations = append(violations, Violation{
				Rule:    "PR_PRIMARY_ISSUE_MISMATCH",
				Message: "Primary Issue must match the pull request Issue number",
			})
		}
	}

	violations = append(violations, ValidateBranch(input.Head, input.Base, input.IssueNumber)...)
	if target := strings.TrimSpace(sections["Target Version"]); target != input.Base {
		violations = append(violations, Violation{
			Rule:    "PR_TARGET_VERSION_MISMATCH",
			Message: "Target Version must match the pull request base branch",
		})
	}

	if pullRequestStatus(input.Body) == "ready" && hasUnverifiedReadyGap(sections) {
		violations = append(violations, Violation{
			Rule:    "PR_READY_WITH_GAPS",
			Message: "A ready pull request cannot report required checks as incomplete",
		})
	}
	return violations
}

func pullRequestStatus(body string) string {
	const prefix = "<!-- agent-policy:status="
	start := strings.Index(body, prefix)
	if start < 0 {
		return ""
	}
	remainder := body[start+len(prefix):]
	end := strings.Index(remainder, " -->")
	if end < 0 {
		return ""
	}
	return strings.ToLower(strings.TrimSpace(remainder[:end]))
}

func hasUnverifiedReadyGap(sections map[string]string) bool {
	for _, heading := range []string{"RED Evidence", "GREEN Evidence", "Regression Verification", "Unverified Items"} {
		value := strings.ToLower(stripMarkdownNonProse(sections[heading]))
		for _, marker := range []string{"not run", "blocked", "failed", "pending", "stale", "not captured", "尚未", "未运行", "失败", "阻塞"} {
			if strings.Contains(value, marker) {
				return true
			}
		}
	}
	return false
}
