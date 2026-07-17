package agentpolicy

import (
	"strings"
	"unicode"
	"unicode/utf8"
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
	var fence byte
	var fenceWidth int

	flush := func() {
		if heading != "" {
			sections[heading] = strings.TrimSpace(strings.Join(content, "\n"))
		}
	}

	for _, line := range strings.Split(strings.ReplaceAll(body, "\r\n", "\n"), "\n") {
		marker, width, isFence := markdownFence(line)
		if fence != 0 {
			if isFence && marker == fence && width >= fenceWidth && fenceCloses(line, marker, width) {
				fence = 0
				fenceWidth = 0
			}
			continue
		}
		if isFence {
			fence = marker
			fenceWidth = width
			continue
		}
		if parsedHeading, ok := policySectionHeading(line); ok {
			flush()
			heading = parsedHeading
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

func policySectionHeading(line string) (string, bool) {
	for _, prefix := range []string{"## ", "### "} {
		if strings.HasPrefix(line, prefix) {
			return strings.TrimSpace(strings.TrimPrefix(line, prefix)), true
		}
	}
	return "", false
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

	emergencyReason := stripMarkdownNonProse(sections["Emergency Reason"])
	if input.IsEmergency && (nonSpaceRuneCount(emergencyReason) < 8 || !containsLetter(emergencyReason)) {
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
	body = stripMarkdownNonProse(stripFencedMarkdown(body))
	for _, line := range strings.Split(body, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "## ") {
			continue
		}
		for _, r := range line {
			if unicode.Is(unicode.Han, r) {
				return true
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
	if len(value) > 3 {
		next, _ := utf8.DecodeRuneInString(value[3:])
		if !unicode.IsSpace(next) && !strings.ContainsRune(":：,，-—", next) {
			return false, ""
		}
	}
	explanation := strings.TrimLeftFunc(value[3:], func(r rune) bool {
		return unicode.IsSpace(r) || strings.ContainsRune(":：,，-—", r)
	})
	return true, explanation
}

func hasImpactDeclaration(value string) bool {
	value = strings.TrimSpace(value)
	for _, declaration := range []string{"yes", "no"} {
		if len(value) < len(declaration) || !strings.EqualFold(value[:len(declaration)], declaration) {
			continue
		}
		remainder := value[len(declaration):]
		if remainder == "" {
			return false
		}
		next, _ := utf8.DecodeRuneInString(remainder)
		if !unicode.IsSpace(next) && !strings.ContainsRune(":：,，-—", next) {
			return false
		}
		reason := strings.TrimLeftFunc(remainder, func(r rune) bool {
			return unicode.IsSpace(r) || strings.ContainsRune(":：,，-—", r)
		})
		return containsLetter(stripMarkdownNonProse(reason))
	}
	return false
}

func stripFencedMarkdown(body string) string {
	var kept []string
	var fence byte
	var fenceWidth int
	for _, line := range strings.Split(strings.ReplaceAll(body, "\r\n", "\n"), "\n") {
		marker, width, isFence := markdownFence(line)
		if fence != 0 {
			if isFence && marker == fence && width >= fenceWidth && fenceCloses(line, marker, width) {
				fence = 0
				fenceWidth = 0
			}
			continue
		}
		if isFence {
			fence = marker
			fenceWidth = width
			continue
		}
		kept = append(kept, line)
	}
	return strings.Join(kept, "\n")
}

func markdownFence(line string) (byte, int, bool) {
	trimmed := strings.TrimLeft(line, " \t")
	if trimmed == "" || trimmed[0] != '`' && trimmed[0] != '~' {
		return 0, 0, false
	}
	marker := trimmed[0]
	width := 0
	for width < len(trimmed) && trimmed[width] == marker {
		width++
	}
	return marker, width, width >= 3
}

func fenceCloses(line string, marker byte, width int) bool {
	trimmed := strings.TrimSpace(line)
	if len(trimmed) < width {
		return false
	}
	for i := 0; i < width; i++ {
		if trimmed[i] != marker {
			return false
		}
	}
	return strings.TrimSpace(trimmed[width:]) == ""
}

func stripMarkdownNonProse(value string) string {
	var output strings.Builder
	inComment := false
	codeDelimiter := 0
	for i := 0; i < len(value); {
		if inComment {
			end := strings.Index(value[i:], "-->")
			if end < 0 {
				break
			}
			i += end + len("-->")
			inComment = false
			continue
		}
		if codeDelimiter == 0 && strings.HasPrefix(value[i:], "<!--") {
			inComment = true
			i += len("<!--")
			continue
		}
		if value[i] == '`' {
			width := 1
			for i+width < len(value) && value[i+width] == '`' {
				width++
			}
			if codeDelimiter == 0 {
				codeDelimiter = width
			} else if width == codeDelimiter {
				codeDelimiter = 0
			}
			i += width
			continue
		}
		if codeDelimiter == 0 {
			output.WriteByte(value[i])
		}
		i++
	}
	return output.String()
}

func containsLetter(value string) bool {
	for _, r := range value {
		if unicode.IsLetter(r) {
			return true
		}
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
