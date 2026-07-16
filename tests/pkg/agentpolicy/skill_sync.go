package agentpolicy

import (
	"path"
	"strings"
	"unicode/utf8"
)

const (
	orientingSkillPrefix = ".agents/skills/chatnow-orienting/"
	coreFlowsReference   = ".agents/skills/chatnow-orienting/references/core-flows.md"
	testingSkillPrefix   = ".agents/skills/chatnow-testing/"
)

// ValidateSkillSync verifies that architecture and core-flow declarations are
// reasoned and that declared impacts update the corresponding repository Skill.
func ValidateSkillSync(input SkillSyncInput) []Violation {
	sections := ParseSections(input.Body)
	architecture := sections["Architecture Impact"]
	coreFlow := sections["Core-flow Impact"]
	files := normalizePaths(input.ChangedFiles)
	var violations []Violation

	if hasArchitectureSensitivePath(files) {
		if !hasImpactDeclaration(architecture) {
			violations = append(violations, Violation{
				Rule:    "SKILL_SYNC_ARCHITECTURE_DECLARATION_REQUIRED",
				Message: "Architecture-sensitive changes require a reasoned Yes or No declaration",
			})
		}
		if !hasImpactDeclaration(coreFlow) {
			violations = append(violations, Violation{
				Rule:    "SKILL_SYNC_CORE_FLOW_DECLARATION_REQUIRED",
				Message: "Architecture-sensitive changes require a reasoned Yes or No core-flow declaration",
			})
		}
	}

	if impactIsYes(architecture) && !hasPathPrefix(files, orientingSkillPrefix) {
		violations = append(violations, Violation{
			Rule:    "SKILL_SYNC_ORIENTING_REQUIRED",
			Message: "Architecture impact requires an update to chatnow-orienting",
		})
	}
	if impactIsYes(coreFlow) && !containsPath(files, coreFlowsReference) {
		violations = append(violations, Violation{
			Rule:    "SKILL_SYNC_CORE_FLOW_REFERENCE_REQUIRED",
			Message: "Core-flow impact requires updating the exact core-flows reference",
		})
	}
	if hasTestArchitecturePath(files) && !hasPathPrefix(files, testingSkillPrefix) {
		violations = append(violations, Violation{
			Rule:    "SKILL_SYNC_TESTING_REQUIRED",
			Message: "Test architecture changes require an update to chatnow-testing",
		})
	}
	return violations
}

func normalizePaths(files []string) []string {
	normalized := make([]string, 0, len(files))
	for _, file := range files {
		file = strings.ReplaceAll(strings.TrimSpace(file), "\\", "/")
		file = strings.TrimPrefix(path.Clean(file), "./")
		if file != "." && file != "" {
			normalized = append(normalized, file)
		}
	}
	return normalized
}

func hasArchitectureSensitivePath(files []string) bool {
	for _, file := range files {
		if strings.HasPrefix(file, "proto/") ||
			strings.Contains(file, "/source/") ||
			strings.HasPrefix(file, "common/infra/") ||
			strings.HasPrefix(file, "common/mq/") ||
			strings.HasPrefix(file, "common/auth/") ||
			strings.HasPrefix(file, "common/dao/") ||
			strings.HasPrefix(file, "conf/") ||
			file == "docker-compose.yml" ||
			strings.HasPrefix(file, "docker/") ||
			file == "CMakeLists.txt" || strings.HasSuffix(file, "/CMakeLists.txt") {
			return true
		}
	}
	return false
}

func hasTestArchitecturePath(files []string) bool {
	for _, file := range files {
		if file == "tests/Makefile" ||
			file == ".github/workflows/ci.yml" ||
			strings.HasPrefix(file, "tests/pkg/framework/") ||
			strings.HasPrefix(file, "tests/pkg/testkit/") {
			return true
		}
	}
	return false
}

func impactIsYes(value string) bool {
	value = strings.TrimSpace(value)
	if len(value) < 3 || !strings.EqualFold(value[:3], "yes") {
		return false
	}
	if len(value) == 3 {
		return true
	}
	next, _ := utf8.DecodeRuneInString(value[3:])
	return next == ' ' || strings.ContainsRune(":：,，-—", next)
}

func containsPath(files []string, wanted string) bool {
	for _, file := range files {
		if file == wanted {
			return true
		}
	}
	return false
}

func hasPathPrefix(files []string, prefix string) bool {
	for _, file := range files {
		if strings.HasPrefix(file, prefix) {
			return true
		}
	}
	return false
}
