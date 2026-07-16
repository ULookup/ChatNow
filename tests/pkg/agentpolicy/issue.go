package agentpolicy

import (
	"strings"
	"unicode"
)

type issueSectionRule struct {
	heading string
	rule    string
}

var issueSectionRules = []issueSectionRule{
	{heading: "Target Version", rule: "ISSUE_TARGET_VERSION_REQUIRED"},
	{heading: "Evidence", rule: "ISSUE_EVIDENCE_REQUIRED"},
	{heading: "Problem or Goal", rule: "ISSUE_PROBLEM_OR_GOAL_REQUIRED"},
	{heading: "Scope", rule: "ISSUE_SCOPE_REQUIRED"},
	{heading: "Non-goals", rule: "ISSUE_NON_GOALS_REQUIRED"},
	{heading: "Acceptance Criteria", rule: "ISSUE_ACCEPTANCE_CRITERIA_REQUIRED"},
	{heading: "Test-first Plan", rule: "ISSUE_TEST_FIRST_PLAN_REQUIRED"},
	{heading: "Risk and Security", rule: "ISSUE_RISK_AND_SECURITY_REQUIRED"},
	{heading: "Architecture Impact", rule: "ISSUE_ARCHITECTURE_IMPACT_REQUIRED"},
	{heading: "Core-flow Impact", rule: "ISSUE_CORE_FLOW_IMPACT_REQUIRED"},
	{heading: "Required Skill Updates", rule: "ISSUE_REQUIRED_SKILL_UPDATES_REQUIRED"},
}

var naAllowedSections = map[string]bool{
	"Non-goals":              true,
	"Risk and Security":      true,
	"Required Skill Updates": true,
}

// ParseSections returns the trimmed content under each level-two Markdown
// heading. If a heading is repeated, the final occurrence wins.
func ParseSections(body string) map[string]string {
	sections := make(map[string]string)
	var heading string
	var content []string

	flush := func() {
		if heading != "" {
			sections[heading] = strings.TrimSpace(strings.Join(content, "\n"))
		}
	}

	for _, line := range strings.Split(strings.ReplaceAll(body, "\r\n", "\n"), "\n") {
		if strings.HasPrefix(line, "## ") {
			flush()
			heading = strings.TrimSpace(strings.TrimPrefix(line, "## "))
			content = nil
			continue
		}
		if heading != "" {
			content = append(content, line)
		}
	}
	flush()
	return sections
}

// ValidateIssue validates the machine-checkable ChatNow Issue contract.
func ValidateIssue(input IssueInput) []Violation {
	var violations []Violation
	if !isEnglishIssueTitle(input.Title) {
		violations = append(violations, Violation{
			Rule:    "ISSUE_TITLE_ENGLISH_REQUIRED",
			Message: "Issue title must contain English letters and printable ASCII only",
		})
	}
	if !containsHanProse(input.Body) {
		violations = append(violations, Violation{
			Rule:    "ISSUE_BODY_CHINESE_REQUIRED",
			Message: "Issue body prose must contain Chinese text",
		})
	}

	sections := ParseSections(input.Body)
	for _, requirement := range issueSectionRules {
		value, present := sections[requirement.heading]
		if !present || !validIssueSection(requirement.heading, value) {
			violations = append(violations, Violation{
				Rule:    requirement.rule,
				Message: "Issue section " + requirement.heading + " is required",
			})
		}
	}
	if strings.TrimSpace(input.TargetVersion) == "" && !hasRuleID(violations, "ISSUE_TARGET_VERSION_REQUIRED") {
		violations = append(violations, Violation{
			Rule:    "ISSUE_TARGET_VERSION_REQUIRED",
			Message: "Issue target version is required",
		})
	}

	for _, heading := range []string{"Architecture Impact", "Core-flow Impact"} {
		if value, ok := sections[heading]; ok && strings.TrimSpace(value) != "" && !hasImpactDeclaration(value) {
			rule := "ISSUE_ARCHITECTURE_IMPACT_REQUIRED"
			if heading == "Core-flow Impact" {
				rule = "ISSUE_CORE_FLOW_IMPACT_REQUIRED"
			}
			if !hasRuleID(violations, rule) {
				violations = append(violations, Violation{
					Rule:    rule,
					Message: "Issue section " + heading + " must declare Yes or No with reasoning",
				})
			}
		}
	}

	if input.IsEmergency && nonSpaceRuneCount(sections["Emergency Reason"]) < 8 {
		violations = append(violations, Violation{
			Rule:    "ISSUE_EMERGENCY_REASON_REQUIRED",
			Message: "Emergency Issue must record why delaying containment increases harm",
		})
	}
	return violations
}

func isEnglishIssueTitle(title string) bool {
	title = strings.TrimSpace(title)
	hasLetter := false
	for _, r := range title {
		if r < 0x20 || r > 0x7e {
			return false
		}
		if r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' {
			hasLetter = true
		}
	}
	return hasLetter
}

func containsHanProse(body string) bool {
	inComment := false
	for _, line := range strings.Split(body, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "## ") {
			continue
		}
		for len(line) > 0 {
			if inComment {
				end := strings.Index(line, "-->")
				if end < 0 {
					line = ""
					continue
				}
				line = line[end+3:]
				inComment = false
			}
			start := strings.Index(line, "<!--")
			prose := line
			if start >= 0 {
				prose = line[:start]
				line = line[start+4:]
				inComment = true
			} else {
				line = ""
			}
			for _, r := range prose {
				if unicode.Is(unicode.Han, r) {
					return true
				}
			}
		}
	}
	return false
}

func validIssueSection(heading, value string) bool {
	value = strings.TrimSpace(value)
	if value == "" {
		return false
	}
	isNA, explanation := naExplanation(value)
	if !isNA {
		return true
	}
	return naAllowedSections[heading] && nonSpaceRuneCount(explanation) >= 8
}

func naExplanation(value string) (bool, string) {
	value = strings.TrimSpace(value)
	if len(value) < 3 || !strings.EqualFold(value[:3], "N/A") {
		return false, ""
	}
	explanation := strings.TrimLeftFunc(value[3:], func(r rune) bool {
		return unicode.IsSpace(r) || strings.ContainsRune(":：-—", r)
	})
	return true, explanation
}

func hasImpactDeclaration(value string) bool {
	value = strings.TrimSpace(value)
	for _, declaration := range []string{"yes", "no"} {
		if len(value) < len(declaration) || !strings.EqualFold(value[:len(declaration)], declaration) {
			continue
		}
		reason := strings.TrimLeftFunc(value[len(declaration):], func(r rune) bool {
			return unicode.IsSpace(r) || strings.ContainsRune(":：,，-—", r)
		})
		return reason != ""
	}
	return false
}

func nonSpaceRuneCount(value string) int {
	count := 0
	for _, r := range value {
		if !unicode.IsSpace(r) {
			count++
		}
	}
	return count
}

func hasRuleID(violations []Violation, rule string) bool {
	for _, violation := range violations {
		if violation.Rule == rule {
			return true
		}
	}
	return false
}
