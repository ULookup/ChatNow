package agentpolicy

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestPullRequestTemplateContract(t *testing.T) {
	content := readRepositoryFile(t, filepath.Join(repositoryRoot(t), ".github", "pull_request_template.md"))
	for _, required := range []string{
		"<!-- agent-policy:status=draft -->",
		"Closes #N",
		"<!-- architecture-impact: yes|no -->",
		"<!-- core-flow-impact: yes|no -->",
		"## Full-diff Self-review",
		"human merge",
	} {
		if !strings.Contains(content, required) {
			t.Errorf("pull request template missing %q", required)
		}
	}
	for _, section := range pullRequestSectionRules {
		if !strings.Contains(content, "## "+section.heading) {
			t.Errorf("pull request template missing heading %q", section.heading)
		}
	}
}

func TestValidatePullRequest(t *testing.T) {
	valid := PullRequestInput{
		Title:        "feat(message): bound push publication retries",
		Body:         validPullRequestBody(),
		Head:         "feat/900-bound-push-retries",
		Base:         "3.0-dev",
		IssueNumber:  900,
		ChangedFiles: []string{"message/source/message_server.h", "tests/func/message_test.go"},
	}

	tests := []struct {
		name      string
		mutate    func(*PullRequestInput)
		wantRules []string
	}{
		{name: "valid Draft"},
		{
			name: "missing primary Issue",
			mutate: func(input *PullRequestInput) {
				input.Body = replaceSection(input.Body, "Primary Issue", "Related #900")
			},
			wantRules: []string{"PR_PRIMARY_ISSUE_REQUIRED"},
		},
		{
			name: "closing Issue mismatch",
			mutate: func(input *PullRequestInput) {
				input.Body = replaceSection(input.Body, "Primary Issue", "Closes #901")
			},
			wantRules: []string{"PR_PRIMARY_ISSUE_MISMATCH"},
		},
		{
			name:      "wrong base",
			mutate:    func(input *PullRequestInput) { input.Base = "3.1-dev" },
			wantRules: []string{"PR_TARGET_VERSION_MISMATCH"},
		},
		{
			name:      "routine main",
			mutate:    func(input *PullRequestInput) { input.Base = "main" },
			wantRules: []string{"BRANCH_BASE_INVALID", "PR_TARGET_VERSION_MISMATCH"},
		},
		{
			name: "English only body",
			mutate: func(input *PullRequestInput) {
				input.Body = strings.NewReplacer(
					"本变更限制推送发布重试。", "This change bounds push publication retries.",
					"不修改客户端协议。", "Client protocols do not change.",
					"因为消息持久化边界不变。", "because the persistence boundary is unchanged.",
					"因为重试属于核心投递流程。", "because retry is part of the delivery flow.",
					"已更新核心流程技能。", "The core-flow Skill is updated.",
					"观察到无限重试断言失败。", "Observed the unbounded retry assertion fail.",
					"聚焦测试通过。", "The focused test passed.",
					"功能回归通过。", "The functional regression passed.",
					"未引入新的凭据或兼容性变化。", "No credential or compatibility change.",
					"没有未验证项目。", "No unverified items.",
					"回滚提交并检查离线箱。", "Revert the commit and inspect the outbox.",
					"没有堆叠依赖。", "No stacked dependency.",
					"已审查 origin/3.0-dev...HEAD 的完整差异，没有无关改动。", "Reviewed the complete origin/3.0-dev...HEAD diff and found no unrelated change.",
				).Replace(input.Body)
			},
			wantRules: []string{"PR_BODY_CHINESE_REQUIRED"},
		},
		{
			name:      "empty RED evidence",
			mutate:    func(input *PullRequestInput) { input.Body = emptySection(input.Body, "RED Evidence") },
			wantRules: []string{"PR_RED_EVIDENCE_REQUIRED"},
		},
		{
			name:      "empty full diff self review",
			mutate:    func(input *PullRequestInput) { input.Body = emptySection(input.Body, "Full-diff Self-review") },
			wantRules: []string{"PR_FULL_DIFF_SELF_REVIEW_REQUIRED"},
		},
		{
			name:      "invalid title type",
			mutate:    func(input *PullRequestInput) { input.Title = "ci: enforce retries" },
			wantRules: []string{"PR_TITLE_INVALID"},
		},
		{
			name:      "non-English title",
			mutate:    func(input *PullRequestInput) { input.Title = "feat: 修" },
			wantRules: []string{"PR_TITLE_INVALID"},
		},
		{
			name: "ready with required check not run",
			mutate: func(input *PullRequestInput) {
				input.Body = strings.Replace(input.Body, "<!-- agent-policy:status=draft -->", "<!-- agent-policy:status=ready -->", 1)
				input.Body = replaceSection(input.Body, "Regression Verification", "NOT RUN：Docker 当前不可用。")
				input.Body = replaceSection(input.Body, "Unverified Items", "Docker 功能回归尚未运行。")
			},
			wantRules: []string{"PR_READY_WITH_GAPS"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			input := valid
			if tt.mutate != nil {
				tt.mutate(&input)
			}
			assertExactRules(t, ValidatePullRequest(input), tt.wantRules)
		})
	}
}

func validPullRequestBody() string {
	return `<!-- agent-policy:status=draft -->
## Primary Issue
Closes #900

## Target Version
3.0-dev

## Scope
本变更限制推送发布重试。

## Non-goals
不修改客户端协议。

## Architecture Impact
No，因为消息持久化边界不变。

## Core-flow Impact
Yes，因为重试属于核心投递流程。

## Updated Skills
已更新核心流程技能。

## RED Evidence
观察到无限重试断言失败。

## GREEN Evidence
聚焦测试通过。

## Regression Verification
功能回归通过。

## Security and Compatibility
未引入新的凭据或兼容性变化。

## Unverified Items
None：没有未验证项目。

## Rollback Plan
回滚提交并检查离线箱。

## Stacked PR Dependencies
None：没有堆叠依赖。

## Full-diff Self-review
已审查 origin/3.0-dev...HEAD 的完整差异，没有无关改动。
`
}
