package agentpolicy

import "testing"

func TestValidateSkillSync(t *testing.T) {
	tests := []struct {
		name      string
		body      string
		files     []string
		wantRules []string
	}{
		{
			name:  "non-sensitive no attestations",
			body:  skillSyncBody("No，因为只改拼写。", "No，因为流程不变。"),
			files: []string{"docs/api/message.md"},
		},
		{
			name:      "sensitive path needs declarations",
			body:      "## Updated Skills\nNone：没有技能变化。\n",
			files:     []string{"message/source/message_server.h"},
			wantRules: []string{"SKILL_SYNC_ARCHITECTURE_DECLARATION_REQUIRED", "SKILL_SYNC_CORE_FLOW_DECLARATION_REQUIRED"},
		},
		{
			name:      "sensitive path needs reasoned declarations",
			body:      skillSyncBody("No", "No."),
			files:     []string{"proto/message/message_service.proto"},
			wantRules: []string{"SKILL_SYNC_ARCHITECTURE_DECLARATION_REQUIRED", "SKILL_SYNC_CORE_FLOW_DECLARATION_REQUIRED"},
		},
		{
			name:      "architecture yes needs orienting update",
			body:      skillSyncBody("Yes，因为服务边界变化。", "No，因为消息流程不变。"),
			files:     []string{"gateway/source/gateway_server.h", "docs/ARCHITECTURE.md"},
			wantRules: []string{"SKILL_SYNC_ORIENTING_REQUIRED"},
		},
		{
			name: "architecture yes with orienting update",
			body: skillSyncBody("Yes，因为服务边界变化。", "No，因为消息流程不变。"),
			files: []string{
				"gateway/source/gateway_server.h",
				".agents\\skills\\chatnow-orienting\\references\\repository-map.md",
			},
		},
		{
			name:      "core flow yes needs exact core flow reference",
			body:      skillSyncBody("No，因为服务边界不变。", "Yes，因为 ACK 顺序变化。"),
			files:     []string{"push/source/push_server.h", ".agents/skills/chatnow-orienting/SKILL.md", "docs/MESSAGE_PIPELINE.md"},
			wantRules: []string{"SKILL_SYNC_CORE_FLOW_REFERENCE_REQUIRED"},
		},
		{
			name: "core flow yes with exact reference",
			body: skillSyncBody("No，因为服务边界不变。", "Yes，因为 ACK 顺序变化。"),
			files: []string{
				"push/source/push_server.h",
				".agents/skills/chatnow-orienting/references/core-flows.md",
			},
		},
		{
			name:      "test architecture needs testing Skill",
			body:      skillSyncBody("No，因为服务边界不变。", "No，因为消息流程不变。"),
			files:     []string{"tests/Makefile", "docs/testing.md"},
			wantRules: []string{"SKILL_SYNC_TESTING_REQUIRED"},
		},
		{
			name: "test architecture with testing Skill",
			body: skillSyncBody("No，因为服务边界不变。", "No，因为消息流程不变。"),
			files: []string{
				"tests/Makefile",
				".agents/skills/chatnow-testing/references/framework.md",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := ValidateSkillSync(SkillSyncInput{Body: tt.body, ChangedFiles: tt.files})
			assertExactRules(t, got, tt.wantRules)
		})
	}
}

func skillSyncBody(architecture, coreFlow string) string {
	return "## Architecture Impact\n" + architecture +
		"\n\n## Core-flow Impact\n" + coreFlow +
		"\n\n## Updated Skills\n列出实际更新的技能路径。\n"
}
