# ChatNow Agent Policy Automation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce ChatNow Issue, branch, commit, PR, verification, Skill-structure, and architecture-sync rules with GitHub templates, a tested Go policy CLI, and GitHub Actions.

**Architecture:** GitHub forms shape agent inputs. Standard-library Go validators turn event data and repository state into structured violations. A small CLI isolates I/O; GitHub Actions builds trusted policy code from the base ref and validates proposed head content without duplicating rules.

**Tech Stack:** Go 1.24 standard library, GitHub Issue Forms, Markdown PR template, GitHub Actions, existing `tests` Go module.

## Global Constraints

- Requires the nine `.agents/skills/chatnow-*` packages.
- Code and machine labels use English; Issue/PR authored prose uses Chinese.
- Implement every validator test-first and observe the expected focused failure.
- Do not add Python, JavaScript, or C++ policy tests.
- Keep validators pure; isolate event, environment, filesystem, and Git access in the CLI.
- Path heuristics trigger semantic declarations but never prove no architecture impact.
- Ordinary task PRs cannot target `main`; release integration is human-controlled.

## Interfaces

```go
type Violation struct { Rule, Message string }
type IssueInput struct { Title, Body, TargetVersion string; IsEmergency bool }
type PullRequestInput struct { Title, Body, Head, Base string; IssueNumber int; ChangedFiles []string }
type SkillSyncInput struct { Body string; ChangedFiles []string }

func ParseSections(body string) map[string]string
func ValidateIssue(IssueInput) []Violation
func ValidateBranch(head, base string, issueNumber int) []Violation
func ValidateCommitSubjects([]string) []Violation
func ValidatePullRequest(PullRequestInput) []Violation
func ValidateSkillSync(SkillSyncInput) []Violation
func ValidateSkillTree(root string) []Violation
```

---

### Task 1: Shared types, section parser, and Issue validation

**Files:** Create `tests/pkg/agentpolicy/{types.go,issue.go,issue_test.go}`.

- [ ] **Step 1: Write RED table tests**

Cover valid English title/Chinese body; missing target, evidence, acceptance, RED plan, or impact declarations; English-only body; and emergency with recorded reason. Assert stable rule IDs.

- [ ] **Step 2: Verify RED**

Run: `cd tests && go test ./pkg/agentpolicy -run 'TestValidateIssue|TestParseSections' -count=1`

Expected: compile failure for missing package/functions.

- [ ] **Step 3: Implement parser and validator**

Required headings are `Target Version`, `Evidence`, `Problem or Goal`, `Scope`, `Non-goals`, `Acceptance Criteria`, `Test-first Plan`, `Risk and Security`, `Architecture Impact`, `Core-flow Impact`, and `Required Skill Updates`. Require printable ASCII English title letters and at least one Han rune in non-marker body prose. Empty values fail. Accept `N/A` only for Non-goals, Risk, or Skill Updates when followed by an explanation of at least eight non-space characters.

- [ ] **Step 4: Verify GREEN and commit**

Run: `cd tests && gofmt -w pkg/agentpolicy && go test ./pkg/agentpolicy -run 'TestValidateIssue|TestParseSections' -count=1`

Then:

```bash
git add tests/pkg/agentpolicy
git commit -m "feat(agent-policy): validate ChatNow Issue contracts"
```

---

### Task 2: Branch and commit validation

**Files:** Create `tests/pkg/agentpolicy/{branch.go,branch_test.go,commits.go,commits_test.go}`.

- [ ] **Step 1: Write RED cases**

Branches: `fix/812-cache-resilience`→`3.1-dev` valid; missing/mismatched Issue invalid; task→`main` invalid; version branch as task head invalid; unsupported prefix invalid. Commits: supported Conventional subject valid; Chinese, WIP, merge, invalid type, uppercase subject, and trailing period invalid.

- [ ] **Step 2: Verify RED**

Run: `cd tests && go test ./pkg/agentpolicy -run 'TestValidateBranch|TestValidateCommitSubjects' -count=1`

- [ ] **Step 3: Implement exact contracts**

Task regex: `^(feat|fix|refactor|test|docs|chore)/([1-9][0-9]*)-[a-z0-9]+(?:-[a-z0-9]+)*$`. Commit types: `feat|fix|refactor|test|docs|chore|build|ci|perf|revert`; optional scope; lowercase ASCII subject start; no trailing period.

- [ ] **Step 4: Verify GREEN and commit**

Run focused tests after `gofmt`, then commit `feat(agent-policy): validate branches and commits` with only the four files.

---

### Task 3: PR and Skill-sync validation

**Files:** Create `tests/pkg/agentpolicy/{pull_request.go,pull_request_test.go,skill_sync.go,skill_sync_test.go}`.

- [ ] **Step 1: Write RED PR cases**

Require headings `Primary Issue`, `Target Version`, `Scope`, `Non-goals`, `Architecture Impact`, `Core-flow Impact`, `Updated Skills`, `RED Evidence`, `GREEN Evidence`, `Regression Verification`, `Security and Compatibility`, `Unverified Items`, `Rollback Plan`, and `Stacked PR Dependencies`. Test missing primary Issue, wrong base, English-only body, empty evidence, and ready status with required checks not run.

- [ ] **Step 2: Write RED Skill-sync cases**

Use design §8.1 paths. Cover no-sensitive `no/no`; sensitive paths need declarations and reasons; architecture `yes` needs `chatnow-orienting`; core-flow `yes` needs `references/core-flows.md`; test architecture needs `chatnow-testing`; reasoned `no` remains an attestation; generic docs alone cannot satisfy `yes`.

- [ ] **Step 3: Verify RED**

Run: `cd tests && go test ./pkg/agentpolicy -run 'TestValidatePullRequest|TestValidateSkillSync' -count=1`

- [ ] **Step 4: Implement and verify GREEN**

Parse `Closes #N`; require branch Issue match and Target Version=base; invoke title/branch validators; require Chinese prose. Normalize slash paths and use exact prefixes; return violations without Git I/O. Run focused tests after `gofmt`.

- [ ] **Step 5: Commit**

Commit `feat(agent-policy): validate pull requests and skill sync` with the four files.

---

### Task 4: Skill tree validation

**Files:** Create `tests/pkg/agentpolicy/{skills.go,skills_test.go}`.

- [ ] **Step 1: Write RED fixture tests with `t.TempDir()`**

Cover exactly nine names; missing `SKILL.md`; name/folder mismatch; extra frontmatter; description not `Use when`; missing `agents/openai.yaml`; default prompt missing `$skill-name`; broken one-level reference; forbidden README/changelog/quick-reference; TODO/TBD/migration wording.

- [ ] **Step 2: Verify RED**

Run: `cd tests && go test ./pkg/agentpolicy -run TestValidateSkillTree -count=1`

- [ ] **Step 3: Implement with standard library only**

Parse only required frontmatter and `openai.yaml` subsets line-by-line. Retain `quick_validate.py` as general validator; this enforces ChatNow-specific policy.

- [ ] **Step 4: Verify GREEN and commit**

Run focused test after `gofmt`; commit `feat(agent-policy): validate repository skill packages`.

---

### Task 5: CLI adapter and Make target

**Files:** Create `tests/cmd/agent-policy/{main.go,main_test.go}`; modify `tests/Makefile`.

- [ ] **Step 1: Write RED command tests**

Test `run(args []string, getenv func(string) string, stdout, stderr io.Writer) int`: unknown command, missing event, valid fixture, GitHub annotation `::error title=RULE::MESSAGE`, and exit 0 success/1 violation/2 usage or I/O.

- [ ] **Step 2: Verify RED**

Run: `cd tests && go test ./cmd/agent-policy -count=1`

- [ ] **Step 3: Implement adapter**

Subcommands: `issue`, `pull-request`, `branch`, `commits`, `skill-sync`, `skills`. Read `GITHUB_EVENT_PATH`; accept newline-delimited `AGENT_POLICY_CHANGED_FILES` and `AGENT_POLICY_COMMIT_SUBJECTS`; default repository root to `..`, overridable by `AGENT_POLICY_REPO_ROOT`.

- [ ] **Step 4: Add Make target**

```make
.PHONY: test-agent-policy
test-agent-policy:
	go test ./pkg/agentpolicy ./cmd/agent-policy -count=1
```

- [ ] **Step 5: Verify GREEN**

Run `gofmt`, `go test ./pkg/agentpolicy ./cmd/agent-policy -count=1`, `go vet ./cmd/agent-policy ./pkg/agentpolicy`, and `make test-agent-policy` from `tests`. Expected: all pass.

- [ ] **Step 6: Commit**

Commit `feat(agent-policy): add policy command interface` with CLI and Makefile only.

---

### Task 6: GitHub Issue Forms

**Files:** Create `.github/ISSUE_TEMPLATE/{config,bug,feature,refactor,engineering}.yml`; update Issue tests.

- [ ] **Step 1: Add RED fixtures matching intended form output**

- [ ] **Step 2: Create `config.yml`**

```yaml
blank_issues_enabled: false
contact_links: []
```

- [ ] **Step 3: Create four forms**

Use English metadata/labels, English prefixes `bug:`, `feat:`, `refactor:`, `engineering:`, and Chinese-writing instructions. Emit every Task 1 heading. Require free-form target version, architecture/core-flow `yes/no` plus reasoning, acceptance prose, and RED plan.

- [ ] **Step 4: Verify and commit**

Run `go test ./pkg/agentpolicy -run TestValidateIssue -count=1` and `git diff --check`; commit `docs(github): add ChatNow agent Issue forms`.

---

### Task 7: Pull request template

**Files:** Create `.github/pull_request_template.md`; update PR tests.

- [ ] **Step 1: Add RED file-contract test**

Read the future template and assert every Task 3 heading and marker exists; observe missing-file failure.

- [ ] **Step 2: Create template**

Use exact headings, English markers `<!-- agent-policy:... -->`, concise Chinese instructions, `Closes #N`, `architecture-impact: yes|no`, `core-flow-impact: yes|no`, and acknowledgements for self-review, Skill sync, honest gaps, Draft status, and human merge.

- [ ] **Step 3: Verify and commit**

Run `go test ./pkg/agentpolicy -run 'TestPullRequestTemplate|TestValidatePullRequest' -count=1`; commit `docs(github): add ChatNow agent pull request template`.

---

### Task 8: Trusted GitHub Actions workflow

**Files:** Create `.github/workflows/agent-policy.yml`; update CLI contract tests.

- [ ] **Step 1: Add RED workflow contract test**

Assert Issue/PR events, full-history checkout, Go 1.24, changed-file/commit collection, and subcommands. Prohibit execution of untrusted head code under `pull_request_target`.

- [ ] **Step 2: Create workflow**

`issue-policy` handles `issues: [opened, edited, reopened]`. `pull-request-policy` handles `pull_request: [opened, edited, synchronize, reopened, ready_for_review]`. Build the policy binary from the base ref, then validate head files and event-derived data without running a head-supplied binary. Collect `base...head` changed files and commit subjects; run branch, commits, PR, Skill-sync, and Skill-tree checks.

- [ ] **Step 3: Verify and commit**

Run all policy tests, `go vet`, and `git diff --check`; commit `ci: enforce ChatNow agent engineering policy`.

---

### Task 9: End-to-end policy verification

**Files:** Modify only artifacts with verified defects.

- [ ] **Step 1: Run local gates**

Run `make test-agent-policy`, policy `go vet`, `gofmt -l` empty check, and `git diff --check`.

- [ ] **Step 2: Exercise all subcommands with temporary valid and invalid Issue/PR JSON**

Expected: valid returns 0; violations return 1 with stable rule IDs. Do not commit fixtures from `/tmp`.

- [ ] **Step 3: Prove Skill-sync failure and recovery**

Changed `message/source/message_server.h` plus `core-flow-impact: yes` and no Skill update returns 1. Adding `.agents/skills/chatnow-orienting/references/core-flows.md` returns 0.

- [ ] **Step 4: Commit observed fixes if present**

```bash
git add .github tests/cmd/agent-policy tests/pkg/agentpolicy tests/Makefile
git diff --cached --quiet || git commit -m "fix(agent-policy): close validation gaps"
```
