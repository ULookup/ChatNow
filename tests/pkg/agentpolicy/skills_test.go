package agentpolicy

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestValidateSkillTree(t *testing.T) {
	tests := []struct {
		name      string
		mutate    func(t *testing.T, root string)
		wantRules []string
	}{
		{name: "valid nine Skill packages"},
		{
			name: "missing SKILL md",
			mutate: func(t *testing.T, root string) {
				removeFixtureFile(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"))
			},
			wantRules: []string{"SKILL_FILE_REQUIRED"},
		},
		{
			name: "frontmatter name differs from folder",
			mutate: func(t *testing.T, root string) {
				replaceFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"), "name: chatnow-testing", "name: chatnow-other")
			},
			wantRules: []string{"SKILL_NAME_MISMATCH"},
		},
		{
			name: "extra frontmatter key",
			mutate: func(t *testing.T, root string) {
				replaceFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"), "description:", "license: MIT\ndescription:")
			},
			wantRules: []string{"SKILL_FRONTMATTER_INVALID"},
		},
		{
			name: "description lacks trigger",
			mutate: func(t *testing.T, root string) {
				replaceFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"), "description: Use when", "description: Guidance for")
			},
			wantRules: []string{"SKILL_DESCRIPTION_INVALID"},
		},
		{
			name: "missing openai yaml",
			mutate: func(t *testing.T, root string) {
				removeFixtureFile(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "agents", "openai.yaml"))
			},
			wantRules: []string{"SKILL_OPENAI_YAML_REQUIRED"},
		},
		{
			name: "default prompt lacks Skill token",
			mutate: func(t *testing.T, root string) {
				replaceFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "agents", "openai.yaml"), "$chatnow-testing", "the testing Skill")
			},
			wantRules: []string{"SKILL_DEFAULT_PROMPT_INVALID"},
		},
		{
			name: "broken one level reference",
			mutate: func(t *testing.T, root string) {
				appendFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"), "\nRead [missing](references/missing.md).\n")
			},
			wantRules: []string{"SKILL_REFERENCE_BROKEN"},
		},
		{
			name: "forbidden companion document",
			mutate: func(t *testing.T, root string) {
				writeFixtureFile(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "README.md"), "duplicate guidance")
			},
			wantRules: []string{"SKILL_FILE_FORBIDDEN"},
		},
		{
			name: "unfinished or historical wording",
			mutate: func(t *testing.T, root string) {
				appendFixtureText(t, filepath.Join(skillFixtureRoot(root), "chatnow-testing", "SKILL.md"), "\nTODO: describe the old test framework migration.\n")
			},
			wantRules: []string{"SKILL_CONTENT_FORBIDDEN"},
		},
		{
			name: "unexpected Skill package",
			mutate: func(t *testing.T, root string) {
				writeFixtureSkill(t, root, "chatnow-extra")
			},
			wantRules: []string{"SKILL_SET_INVALID"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := t.TempDir()
			writeValidSkillTree(t, root)
			if tt.mutate != nil {
				tt.mutate(t, root)
			}

			got := ValidateSkillTree(root)
			if tt.wantRules == nil {
				if len(got) != 0 {
					t.Fatalf("ValidateSkillTree() violations = %#v, want none", got)
				}
				return
			}
			for _, rule := range tt.wantRules {
				if !hasRule(got, rule) {
					t.Errorf("ValidateSkillTree() rules = %v, want %q", violationRules(got), rule)
				}
			}
		})
	}
}

func TestValidateRepositorySkillTree(t *testing.T) {
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	if got := ValidateSkillTree(root); len(got) != 0 {
		t.Fatalf("repository Skill violations = %#v", got)
	}
}

func writeValidSkillTree(t *testing.T, root string) {
	t.Helper()
	for _, name := range requiredSkillNames {
		writeFixtureSkill(t, root, name)
	}
	writeFixtureFile(t, filepath.Join(skillFixtureRoot(root), "chatnow-orienting", "references", "core-flows.md"), "# Core flows\n")
}

func writeFixtureSkill(t *testing.T, root, name string) {
	t.Helper()
	skill := "---\nname: " + name + "\ndescription: Use when an agent needs this ChatNow workflow\n---\n\n# Skill\n"
	yaml := "interface:\n  display_name: \"Fixture\"\n  short_description: \"Fixture Skill\"\n  default_prompt: \"Use $" + name + " for this task.\"\n"
	writeFixtureFile(t, filepath.Join(skillFixtureRoot(root), name, "SKILL.md"), skill)
	writeFixtureFile(t, filepath.Join(skillFixtureRoot(root), name, "agents", "openai.yaml"), yaml)
}

func skillFixtureRoot(root string) string {
	return filepath.Join(root, ".agents", "skills")
}

func writeFixtureFile(t *testing.T, file, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(file), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func removeFixtureFile(t *testing.T, file string) {
	t.Helper()
	if err := os.Remove(file); err != nil {
		t.Fatal(err)
	}
}

func replaceFixtureText(t *testing.T, file, old, replacement string) {
	t.Helper()
	content, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	updated := strings.Replace(string(content), old, replacement, 1)
	if updated == string(content) {
		t.Fatalf("fixture %s does not contain %q", file, old)
	}
	writeFixtureFile(t, file, updated)
}

func appendFixtureText(t *testing.T, file, addition string) {
	t.Helper()
	content, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	writeFixtureFile(t, file, string(content)+addition)
}
