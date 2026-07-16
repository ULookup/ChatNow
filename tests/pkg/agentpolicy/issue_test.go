package agentpolicy

import (
	"strings"
	"testing"
)

func TestParseSections(t *testing.T) {
	body := "intro marker\n## Target Version\n\n3.1-dev\ncontinued\n## Evidence\n\n`issue.go` 缺少校验。\n## Empty\n## Target Version\n4.0-dev\n"

	sections := ParseSections(body)

	if got, want := sections["Target Version"], "4.0-dev"; got != want {
		t.Fatalf("Target Version = %q, want %q", got, want)
	}
	if got, want := sections["Evidence"], "`issue.go` 缺少校验。"; got != want {
		t.Fatalf("Evidence = %q, want %q", got, want)
	}
	if got := sections["Empty"]; got != "" {
		t.Fatalf("Empty = %q, want empty", got)
	}
}

func TestValidateIssue(t *testing.T) {
	valid := IssueInput{
		Title:         "Validate Issue contracts",
		Body:          validIssueBody(),
		TargetVersion: "3.1-dev",
	}

	tests := []struct {
		name      string
		mutate    func(*IssueInput)
		wantRules []string
	}{
		{name: "valid English title and Chinese body"},
		{
			name: "missing target input",
			mutate: func(input *IssueInput) {
				input.TargetVersion = ""
			},
			wantRules: []string{"ISSUE_TARGET_VERSION_REQUIRED"},
		},
		{
			name: "missing target section",
			mutate: func(input *IssueInput) {
				input.Body = removeSection(input.Body, "Target Version")
			},
			wantRules: []string{"ISSUE_TARGET_VERSION_REQUIRED"},
		},
		{
			name: "missing evidence",
			mutate: func(input *IssueInput) {
				input.Body = emptySection(input.Body, "Evidence")
			},
			wantRules: []string{"ISSUE_EVIDENCE_REQUIRED"},
		},
		{
			name: "missing acceptance criteria",
			mutate: func(input *IssueInput) {
				input.Body = emptySection(input.Body, "Acceptance Criteria")
			},
			wantRules: []string{"ISSUE_ACCEPTANCE_CRITERIA_REQUIRED"},
		},
		{
			name: "missing RED plan",
			mutate: func(input *IssueInput) {
				input.Body = emptySection(input.Body, "Test-first Plan")
			},
			wantRules: []string{"ISSUE_TEST_FIRST_PLAN_REQUIRED"},
		},
		{
			name: "missing architecture declaration",
			mutate: func(input *IssueInput) {
				input.Body = emptySection(input.Body, "Architecture Impact")
			},
			wantRules: []string{"ISSUE_ARCHITECTURE_IMPACT_REQUIRED"},
		},
		{
			name: "missing core flow declaration",
			mutate: func(input *IssueInput) {
				input.Body = emptySection(input.Body, "Core-flow Impact")
			},
			wantRules: []string{"ISSUE_CORE_FLOW_IMPACT_REQUIRED"},
		},
		{
			name: "English only body",
			mutate: func(input *IssueInput) {
				input.Body = strings.NewReplacer(
					"校验器必须拒绝不完整的 Issue。", "The validator must reject incomplete issues.",
					"实现 Issue 合同校验。", "Implement issue contract validation.",
					"只修改策略包。", "Only change the policy package.",
					"不修改运行时服务。", "Do not change runtime services.",
					"缺失字段时返回稳定规则编号。", "Return stable rule IDs for missing fields.",
					"先运行聚焦测试并观察失败。", "Run the focused test and observe RED first.",
					"无安全风险，因为只解析文本。", "No security risk because this only parses text.",
					"No，因为不改变服务边界。", "No, because service boundaries do not change.",
					"No，因为不改变核心消息流程。", "No, because the core message flow does not change.",
					"N/A：现有技能说明已经覆盖此规则。", "N/A: existing skills already cover this rule.",
				).Replace(input.Body)
			},
			wantRules: []string{"ISSUE_BODY_CHINESE_REQUIRED"},
		},
		{
			name: "non ASCII title",
			mutate: func(input *IssueInput) {
				input.Title = "校验 Issue contracts"
			},
			wantRules: []string{"ISSUE_TITLE_ENGLISH_REQUIRED"},
		},
		{
			name: "N A in required evidence",
			mutate: func(input *IssueInput) {
				input.Body = replaceSection(input.Body, "Evidence", "N/A：没有可以记录的证据。")
			},
			wantRules: []string{"ISSUE_EVIDENCE_REQUIRED"},
		},
		{
			name: "N A without a sufficient explanation",
			mutate: func(input *IssueInput) {
				input.Body = replaceSection(input.Body, "Non-goals", "N/A：无。")
			},
			wantRules: []string{"ISSUE_NON_GOALS_REQUIRED"},
		},
		{
			name: "emergency without recorded reason",
			mutate: func(input *IssueInput) {
				input.IsEmergency = true
			},
			wantRules: []string{"ISSUE_EMERGENCY_REASON_REQUIRED"},
		},
		{
			name: "emergency with recorded reason",
			mutate: func(input *IssueInput) {
				input.IsEmergency = true
				input.Body += "\n## Emergency Reason\n\n延迟修复会扩大凭据泄露风险，先执行最小安全遏制。\n"
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			input := valid
			if tt.mutate != nil {
				tt.mutate(&input)
			}

			got := ValidateIssue(input)
			if tt.wantRules == nil {
				if len(got) != 0 {
					t.Fatalf("ValidateIssue() violations = %#v, want none", got)
				}
				return
			}
			for _, rule := range tt.wantRules {
				if !hasRule(got, rule) {
					t.Errorf("ValidateIssue() rules = %v, want %q", violationRules(got), rule)
				}
			}
		})
	}
}

func validIssueBody() string {
	return `## Target Version

3.1-dev

## Evidence

校验器必须拒绝不完整的 Issue。

## Problem or Goal

实现 Issue 合同校验。

## Scope

只修改策略包。

## Non-goals

不修改运行时服务。

## Acceptance Criteria

- [ ] 缺失字段时返回稳定规则编号。

## Test-first Plan

先运行聚焦测试并观察失败。

## Risk and Security

无安全风险，因为只解析文本。

## Architecture Impact

No，因为不改变服务边界。

## Core-flow Impact

No，因为不改变核心消息流程。

## Required Skill Updates

N/A：现有技能说明已经覆盖此规则。
`
}

func removeSection(body, heading string) string {
	start := strings.Index(body, "## "+heading+"\n")
	if start < 0 {
		return body
	}
	rest := body[start+len("## "+heading+"\n"):]
	next := strings.Index(rest, "\n## ")
	if next < 0 {
		return body[:start]
	}
	return body[:start] + rest[next+1:]
}

func emptySection(body, heading string) string {
	return replaceSection(body, heading, "")
}

func replaceSection(body, heading, value string) string {
	start := strings.Index(body, "## "+heading+"\n")
	if start < 0 {
		return body
	}
	contentStart := start + len("## "+heading+"\n")
	rest := body[contentStart:]
	next := strings.Index(rest, "\n## ")
	if next < 0 {
		return body[:contentStart] + "\n" + value + "\n"
	}
	return body[:contentStart] + "\n" + value + rest[next:]
}

func hasRule(violations []Violation, rule string) bool {
	for _, violation := range violations {
		if violation.Rule == rule {
			return true
		}
	}
	return false
}

func violationRules(violations []Violation) []string {
	rules := make([]string, 0, len(violations))
	for _, violation := range violations {
		rules = append(rules, violation.Rule)
	}
	return rules
}
