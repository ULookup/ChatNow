package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"regexp"
	"strconv"
	"strings"

	"chatnow-tests/pkg/agentpolicy"
)

type githubEvent struct {
	Issue struct {
		Title  string `json:"title"`
		Body   string `json:"body"`
		Labels []struct {
			Name string `json:"name"`
		} `json:"labels"`
	} `json:"issue"`
	PullRequest struct {
		Title string `json:"title"`
		Body  string `json:"body"`
		Head  struct {
			Ref string `json:"ref"`
		} `json:"head"`
		Base struct {
			Ref string `json:"ref"`
		} `json:"base"`
	} `json:"pull_request"`
}

var commandBranchPattern = regexp.MustCompile(`^(?:feat|fix|refactor|test|docs|chore)/([1-9][0-9]*)-`)

func main() {
	os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr))
}

func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	if len(args) != 1 {
		fmt.Fprintln(stderr, "usage: agent-policy <issue|pull-request|branch|commits|skill-sync|skills>")
		return 2
	}

	command := args[0]
	known := map[string]bool{
		"issue": true, "pull-request": true, "branch": true,
		"commits": true, "skill-sync": true, "skills": true,
	}
	if !known[command] {
		fmt.Fprintf(stderr, "unknown command %q\n", command)
		return 2
	}

	var violations []agentpolicy.Violation
	switch command {
	case "commits":
		violations = agentpolicy.ValidateCommitSubjects(splitLines(getenv("AGENT_POLICY_COMMIT_SUBJECTS")))
	case "skills":
		root := strings.TrimSpace(getenv("AGENT_POLICY_REPO_ROOT"))
		if root == "" {
			root = ".."
		}
		violations = agentpolicy.ValidateSkillTree(root)
	default:
		event, err := readEvent(getenv("GITHUB_EVENT_PATH"))
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 2
		}
		changedFiles := splitLines(getenv("AGENT_POLICY_CHANGED_FILES"))
		switch command {
		case "issue":
			sections := agentpolicy.ParseSections(event.Issue.Body)
			violations = agentpolicy.ValidateIssue(agentpolicy.IssueInput{
				Title:         event.Issue.Title,
				Body:          event.Issue.Body,
				TargetVersion: strings.TrimSpace(sections["Target Version"]),
				IsEmergency:   hasEmergencyLabel(event),
			})
		case "pull-request":
			issueNumber := issueNumberFromBranch(event.PullRequest.Head.Ref)
			violations = agentpolicy.ValidatePullRequest(agentpolicy.PullRequestInput{
				Title:        event.PullRequest.Title,
				Body:         event.PullRequest.Body,
				Head:         event.PullRequest.Head.Ref,
				Base:         event.PullRequest.Base.Ref,
				IssueNumber:  issueNumber,
				ChangedFiles: changedFiles,
			})
		case "branch":
			violations = agentpolicy.ValidateBranch(
				event.PullRequest.Head.Ref,
				event.PullRequest.Base.Ref,
				issueNumberFromBranch(event.PullRequest.Head.Ref),
			)
		case "skill-sync":
			violations = agentpolicy.ValidateSkillSync(agentpolicy.SkillSyncInput{
				Body:         event.PullRequest.Body,
				ChangedFiles: changedFiles,
			})
		}
	}

	for _, violation := range violations {
		fmt.Fprintf(stdout, "::error title=%s::%s\n", escapeAnnotation(violation.Rule), escapeAnnotation(violation.Message))
	}
	if len(violations) > 0 {
		return 1
	}
	return 0
}

func readEvent(file string) (githubEvent, error) {
	var event githubEvent
	if strings.TrimSpace(file) == "" {
		return event, fmt.Errorf("GITHUB_EVENT_PATH is required")
	}
	content, err := os.ReadFile(file)
	if err != nil {
		return event, fmt.Errorf("read GITHUB_EVENT_PATH: %w", err)
	}
	if err := json.Unmarshal(content, &event); err != nil {
		return event, fmt.Errorf("parse GITHUB_EVENT_PATH: %w", err)
	}
	return event, nil
}

func splitLines(value string) []string {
	var lines []string
	for _, line := range strings.Split(strings.ReplaceAll(value, "\r\n", "\n"), "\n") {
		if line = strings.TrimSpace(line); line != "" {
			lines = append(lines, line)
		}
	}
	return lines
}

func issueNumberFromBranch(branch string) int {
	matches := commandBranchPattern.FindStringSubmatch(branch)
	if matches == nil {
		return 0
	}
	number, _ := strconv.Atoi(matches[1])
	return number
}

func hasEmergencyLabel(event githubEvent) bool {
	for _, label := range event.Issue.Labels {
		if strings.EqualFold(strings.TrimSpace(label.Name), "emergency") {
			return true
		}
	}
	return false
}

func escapeAnnotation(value string) string {
	value = strings.ReplaceAll(value, "%", "%25")
	value = strings.ReplaceAll(value, "\r", "%0D")
	return strings.ReplaceAll(value, "\n", "%0A")
}
