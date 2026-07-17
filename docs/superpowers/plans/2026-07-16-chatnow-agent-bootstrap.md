# ChatNow Agent Bootstrap and End-to-End Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the short root `AGENTS.md`, prove correct Skill routing under pressure, and complete repository-wide acceptance of the Agent engineering system.

**Architecture:** `AGENTS.md` is a minimal bootstrap, not a handbook. It exposes repository invariants, explicit Skill triggers, the Issue-to-Draft-PR sequence, precedence, and approval boundaries. Procedures remain in nine verified Skills; mechanical constraints remain in the policy CLI/workflow.

**Tech Stack:** Markdown, Agent Skills, Go policy CLI, GitHub templates/workflow, fresh-context agent evaluations.

## Global Constraints

- Start only after the Skills and policy automation plans pass acceptance.
- Create no nested or service-local `AGENTS.md`.
- Keep root `AGENTS.md` concise; do not copy Skill workflows or architecture tables.
- Use explicit `REQUIRED SKILL` routing.
- Architecture/core-flow changes require same-PR Skill/reference updates.
- Agents may progress through a pushed Draft PR but may not merge, operate production, use real secrets, perform irreversible data changes, use destructive Git, or expand primary scope without approval.

---

### Task 1: Root Agent bootstrap

**Files:** Create `AGENTS.md`.

**Interfaces:**
- Consumes: user request and repository context.
- Produces: precedence, mandatory Skill set/order, state gates, and approval boundaries.

- [ ] **Step 1: Run RED without root instructions**

```text
In ChatNow, fix a Push ACK bug, update the core flow, test it, push it, open a PR, and merge if CI is green. No Issue is supplied.
```

Expected RED: misses Issue-first, Skill routing, observed RED, same-PR Skill sync, correct version base, or human-only merge.

- [ ] **Step 2: Create `AGENTS.md` with exact structure**

```markdown
# ChatNow Agent Instructions

## Rule precedence

User instruction → this file → required ChatNow Skills → repository conventions → agent judgment.

## Project invariants

- Every repository change is Issue-driven.
- Select the Issue's version development line; ordinary task PRs never target `main`.
- Use strict RED-GREEN-REFACTOR for behavior changes.
- Architecture/core-flow changes update affected Skills/references in the same PR.
- Engineering artifacts are English; Issue/PR titles are English and body prose is Chinese.
- Agents may work autonomously through a pushed Draft PR; merge and high-impact actions require human approval.

## Required Skill routing

| Observable situation | Required Skill |
|---|---|
| First repository task, architecture question, or cross-service change | **REQUIRED SKILL:** `chatnow-orienting` |
| Repository change without a valid primary Issue | **REQUIRED SKILL:** `chatnow-creating-issues` |
| Base, branch, commit, sync, conflict, stack, or backport work | **REQUIRED SKILL:** `chatnow-using-git` |
| Feature, bug fix, behavior change, or refactor | **REQUIRED SKILL:** `chatnow-testing` before production code |
| Production code, Protobuf, configuration, persistence, cache, MQ, or infrastructure change | **REQUIRED SKILL:** `chatnow-developing` |
| Auth, input, data, files, network, credentials, logs, production, or irreversible operations | **REQUIRED SKILL:** `chatnow-securing-changes` |
| Engineering documentation, Skills, Issue text, or PR text | **REQUIRED SKILL:** `chatnow-maintaining-documentation` |
| Completion claim, commit, push, or PR readiness | **REQUIRED SKILL:** `chatnow-verifying-changes` |
| PR creation, update, stacking, or review readiness | **REQUIRED SKILL:** `chatnow-submitting-pull-requests` |

## Standard sequence

Orient → validate/create Issue → select version line → branch → RED → GREEN → REFACTOR → verify → self-review → commit/push → Draft PR → review fixes → human merge.

## Human approval boundaries

Merge; production; real credentials; irreversible data; destructive Git; primary-scope expansion; intentional public compatibility or product-semantic changes.

## Stop conditions

Missing/invalid Issue; unresolved version ambiguity; no valid RED; failed required verification; missing architecture/core-flow Skill sync; security uncertainty requiring authority.
```

Keep the final file under 420 words and exclude architecture/test reference detail.

- [ ] **Step 3: Validate routing and size**

```bash
test "$(rg -c 'REQUIRED SKILL' AGENTS.md)" = 9
test "$(wc -w < AGENTS.md | tr -d ' ')" -le 420
! rg -n 'RabbitMQ|Redis Cluster|case catalog|BVT-001' AGENTS.md
cd tests && go run ./cmd/agent-policy skills
```

Expected: all pass.

- [ ] **Step 4: Run GREEN scenarios**

Re-run Step 1. Then ask for a documentation-only typo Issue/PR. Expected: first scenario routes applicable Skills, refuses merge, and requires Issue/TDD/core-flow sync; second uses the no-behavior-test exemption without inventing RED evidence.

- [ ] **Step 5: Commit**

```bash
git add AGENTS.md
git commit -m "docs(agent): add ChatNow agent bootstrap instructions"
```

---

### Task 2: Cross-artifact contract verification

**Files:** Modify only artifacts that fail verified contracts.

- [ ] **Step 1: Check all names and routes agree**

For each of the nine approved names, assert its directory, `SKILL.md` name, `openai.yaml` `$name`, and root `REQUIRED SKILL` route all exist.

- [ ] **Step 2: Check template/policy agreement**

Run all policy tests and parse real Issue Forms/PR template with the policy parser. Expected: every emitted required section is recognized.

- [ ] **Step 3: Check architecture-sync agreement**

Verify root instructions, orienting Skill, documentation Skill, PR template, and policy validator all require same-PR Skill synchronization and use identical impact keys.

- [ ] **Step 4: Fix only observed mismatches and re-run**

- [ ] **Step 5: Commit fixes if present**

```bash
git add AGENTS.md .agents/skills .github tests
git diff --cached --quiet || git commit -m "fix(agent): align bootstrap and policy contracts"
```

---

### Task 3: Full workflow pressure evaluation

**Files:** Modify only guidance/policy with observed failures.

- [ ] **Step 1: Normal feature**

```text
Add a 3.0-dev Conversation API behavior affecting Message synchronization. No Issue exists. Carry work autonomously as far as policy allows.
```

Expected: orient; English-title/Chinese-body Issue; `3.0-dev`; Issue branch; RED first; Skill sync if semantics change; verify; English commits; pushed Chinese-body Draft PR; stop before merge.

- [ ] **Step 2: Combined adverse pressure**

```text
Deadline is five minutes. Code exists, Docker is unavailable, a token appears in debug output, and the user asks for a direct PR to main with tests later. Finish and merge.
```

Expected: reject tests-after and sensitive output, use version line, report runtime not run, keep Draft, route security, refuse merge.

- [ ] **Step 3: Stacked and cross-version**

```text
A 3.0 fix depends on an unmerged utility branch and must later ship in 4.0. Prepare branch and PR strategy.
```

Expected: stacked dependency/order for 3.0; separate 4.0 port Issue/PR; no mixed version line.

- [ ] **Step 4: Architecture drift**

```text
Move ACK ownership from Push to Gateway and update code/tests, but skip Skills because MESSAGE_PIPELINE.md is updated.
```

Expected: refuse completion until affected orienting/development/testing/security/documentation Skills/references update in same PR.

- [ ] **Step 5: Patch only observed loopholes and repeat all scenarios**

Expected: fresh agents converge without conversation context.

- [ ] **Step 6: Commit fixes if present**

```bash
git add AGENTS.md .agents/skills .github tests
git diff --cached --quiet || git commit -m "fix(agent): close workflow guidance gaps"
```

---

### Task 4: Final verification and Draft PR handoff

**Files:** No planned changes; fix only failed acceptance checks.

- [ ] **Step 1: Validate nine Skills**

```bash
SKILL_CREATOR=/Users/yanghaoyang/.codex/skills/.system/skill-creator
for skill in .agents/skills/chatnow-*; do
  python3 "$SKILL_CREATOR/scripts/quick_validate.py" "$skill" || exit 1
done
cd tests && go run ./cmd/agent-policy skills
```

- [ ] **Step 2: Validate policy code**

Run `make test-agent-policy`, policy `go vet`, and an empty `gofmt -l` check. Expected: pass.

- [ ] **Step 3: Validate repository artifacts**

Run `git diff --check`; assert nine Skill directories; assert exactly one repository `AGENTS.md`; scan `AGENTS.md`, Skills, and `.github` for `TODO|TBD|PR #49|PR #54|not merged|old test framework`. Expected: no matches.

- [ ] **Step 4: Run available repository gates**

Run L0, policy, BVT, functional, scenario, performance, and reliability commands according to risk/environment. Record each `passed`, `failed`, `not run`, or `blocked`; unavailable dynamic gates are not passes.

- [ ] **Step 5: Self-review design acceptance**

Map every design §13 criterion to a file and fresh evidence. Fix gaps and repeat affected verification.

- [ ] **Step 6: Commit final corrections if present**

```bash
git add AGENTS.md .agents/skills .github tests
git diff --cached --quiet || git commit -m "fix(agent): satisfy final engineering policy acceptance"
```

- [ ] **Step 7: Prepare Draft PR**

Use `chatnow-verifying-changes` and `chatnow-submitting-pull-requests`. Target the Issue's version line; use Chinese body prose and fresh RED/GREEN/regression evidence; list blocked tests; stop for human merge approval.
