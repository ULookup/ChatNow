package agentpolicy

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestIssueFormContracts(t *testing.T) {
	repoRoot := repositoryRoot(t)
	templateDir := filepath.Join(repoRoot, ".github", "ISSUE_TEMPLATE")
	config := readRepositoryFile(t, filepath.Join(templateDir, "config.yml"))
	for _, required := range []string{"blank_issues_enabled: false", "contact_links: []"} {
		if !strings.Contains(config, required) {
			t.Errorf("config.yml missing %q", required)
		}
	}

	forms := map[string]string{
		"bug.yml":         "bug: ",
		"feature.yml":     "feat: ",
		"refactor.yml":    "refactor: ",
		"engineering.yml": "engineering: ",
	}
	for file, prefix := range forms {
		t.Run(file, func(t *testing.T) {
			content := readRepositoryFile(t, filepath.Join(templateDir, file))
			for _, required := range []string{
				"name:", "description:", "title: \"" + prefix + "\"", "labels:", "body:",
				"Write the response in Chinese", "validations:", "required: true",
			} {
				if !strings.Contains(content, required) {
					t.Errorf("%s missing %q", file, required)
				}
			}
			for _, heading := range issueSectionRules {
				if !strings.Contains(content, "label: "+heading.heading) {
					t.Errorf("%s missing field label %q", file, heading.heading)
				}
			}
		})
	}
}

func TestIssueFormOutputValidates(t *testing.T) {
	body := strings.ReplaceAll(validIssueBody(), "## ", "### ")
	got := ValidateIssue(IssueInput{
		Title:         "fix: validate generated Issue forms",
		Body:          body,
		TargetVersion: "3.1-dev",
	})
	if len(got) != 0 {
		t.Fatalf("Issue Form output violations = %#v", got)
	}
}

func repositoryRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return root
}

func readRepositoryFile(t *testing.T, file string) string {
	t.Helper()
	content, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	return string(content)
}
