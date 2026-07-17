package agentpolicy

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

var requiredSkillNames = []string{
	"chatnow-creating-issues",
	"chatnow-developing",
	"chatnow-maintaining-documentation",
	"chatnow-orienting",
	"chatnow-securing-changes",
	"chatnow-submitting-pull-requests",
	"chatnow-testing",
	"chatnow-using-git",
	"chatnow-verifying-changes",
}

var skillReferencePattern = regexp.MustCompile(`\]\((references/[^\s)#?]+)(?:#[^)]+)?\)`)

// ValidateSkillTree validates the ChatNow-specific repository Skill contract
// below root/.agents/skills.
func ValidateSkillTree(root string) []Violation {
	skillsRoot := filepath.Join(root, ".agents", "skills")
	entries, err := os.ReadDir(skillsRoot)
	if err != nil {
		return []Violation{{Rule: "SKILL_TREE_UNREADABLE", Message: "Cannot read .agents/skills: " + err.Error()}}
	}

	var violations []Violation
	var actual []string
	for _, entry := range entries {
		if entry.IsDir() {
			actual = append(actual, entry.Name())
		}
	}
	sort.Strings(actual)
	if !equalStrings(actual, requiredSkillNames) {
		violations = append(violations, Violation{
			Rule:    "SKILL_SET_INVALID",
			Message: "Repository must contain exactly the nine required ChatNow Skill packages",
		})
	}

	for _, name := range requiredSkillNames {
		skillDir := filepath.Join(skillsRoot, name)
		if info, statErr := os.Stat(skillDir); statErr != nil || !info.IsDir() {
			continue
		}
		violations = append(violations, validateSkillPackage(skillDir, name)...)
	}
	return violations
}

func validateSkillPackage(skillDir, folderName string) []Violation {
	var violations []Violation
	skillFile := filepath.Join(skillDir, "SKILL.md")
	content, err := os.ReadFile(skillFile)
	if err != nil {
		violations = append(violations, Violation{
			Rule:    "SKILL_FILE_REQUIRED",
			Message: folderName + "/SKILL.md is required",
		})
	} else {
		frontmatter, valid := parseSkillFrontmatter(string(content))
		if !valid {
			violations = append(violations, Violation{
				Rule:    "SKILL_FRONTMATTER_INVALID",
				Message: folderName + " frontmatter must contain only name and description",
			})
		} else {
			if frontmatter["name"] != folderName {
				violations = append(violations, Violation{
					Rule:    "SKILL_NAME_MISMATCH",
					Message: folderName + " frontmatter name must match its folder",
				})
			}
			description := frontmatter["description"]
			if !strings.HasPrefix(description, "Use when") && !strings.HasPrefix(description, "Use before") {
				violations = append(violations, Violation{
					Rule:    "SKILL_DESCRIPTION_INVALID",
					Message: folderName + " description must state a Use when or Use before trigger",
				})
			}
		}
		violations = append(violations, validateSkillReferences(skillDir, string(content))...)
	}

	openAIFile := filepath.Join(skillDir, "agents", "openai.yaml")
	openAI, err := os.ReadFile(openAIFile)
	if err != nil {
		violations = append(violations, Violation{
			Rule:    "SKILL_OPENAI_YAML_REQUIRED",
			Message: folderName + "/agents/openai.yaml is required",
		})
	} else if !defaultPromptContainsSkill(string(openAI), folderName) {
		violations = append(violations, Violation{
			Rule:    "SKILL_DEFAULT_PROMPT_INVALID",
			Message: folderName + " default_prompt must mention $" + folderName,
		})
	}

	_ = filepath.WalkDir(skillDir, func(file string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			violations = append(violations, Violation{
				Rule:    "SKILL_TREE_UNREADABLE",
				Message: "Cannot inspect " + file + ": " + walkErr.Error(),
			})
			return nil
		}
		if entry.IsDir() {
			return nil
		}
		if forbiddenSkillFilename(entry.Name()) {
			violations = append(violations, Violation{
				Rule:    "SKILL_FILE_FORBIDDEN",
				Message: "Skill package contains forbidden companion document " + entry.Name(),
			})
		}
		fileContent, readErr := os.ReadFile(file)
		if readErr == nil && containsForbiddenSkillWording(string(fileContent)) {
			violations = append(violations, Violation{
				Rule:    "SKILL_CONTENT_FORBIDDEN",
				Message: "Skill package contains unfinished or superseded-state wording",
			})
		}
		return nil
	})
	return violations
}

func parseSkillFrontmatter(content string) (map[string]string, bool) {
	lines := strings.Split(strings.ReplaceAll(content, "\r\n", "\n"), "\n")
	if len(lines) < 4 || lines[0] != "---" {
		return nil, false
	}
	values := make(map[string]string)
	closed := false
	for _, line := range lines[1:] {
		if line == "---" {
			closed = true
			break
		}
		key, value, ok := strings.Cut(line, ":")
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		if !ok || value == "" || key != "name" && key != "description" {
			return nil, false
		}
		if _, exists := values[key]; exists {
			return nil, false
		}
		values[key] = value
	}
	return values, closed && len(values) == 2
}

func defaultPromptContainsSkill(content, name string) bool {
	for _, line := range strings.Split(strings.ReplaceAll(content, "\r\n", "\n"), "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "default_prompt:") {
			return strings.Contains(trimmed, "$"+name)
		}
	}
	return false
}

func validateSkillReferences(skillDir, content string) []Violation {
	var violations []Violation
	for _, match := range skillReferencePattern.FindAllStringSubmatch(content, -1) {
		reference := filepath.FromSlash(match[1])
		if strings.Contains(reference, ".."+string(filepath.Separator)) {
			violations = append(violations, Violation{Rule: "SKILL_REFERENCE_BROKEN", Message: "Skill reference must remain inside its package"})
			continue
		}
		if info, err := os.Stat(filepath.Join(skillDir, reference)); err != nil || info.IsDir() {
			violations = append(violations, Violation{
				Rule:    "SKILL_REFERENCE_BROKEN",
				Message: fmt.Sprintf("Skill reference %s does not resolve", match[1]),
			})
		}
	}
	return violations
}

func forbiddenSkillFilename(name string) bool {
	normalized := strings.ToLower(strings.NewReplacer("_", "-", " ", "-").Replace(name))
	return normalized == "readme.md" ||
		strings.HasPrefix(normalized, "changelog") ||
		strings.HasPrefix(normalized, "quick-reference")
}

func containsForbiddenSkillWording(content string) bool {
	lower := strings.ToLower(content)
	return regexp.MustCompile(`(?m)\b(todo|tbd)\b`).MatchString(lower) ||
		strings.Contains(lower, "not merged") ||
		strings.Contains(lower, "pr #49") ||
		strings.Contains(lower, "pr #54") ||
		strings.Contains(lower, "old test framework")
}

func equalStrings(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}
