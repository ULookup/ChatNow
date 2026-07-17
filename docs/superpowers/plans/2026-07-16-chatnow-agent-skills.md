# ChatNow Agent Skills Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and behaviorally validate the nine repository-local ChatNow engineering Skills and their project references.

**Architecture:** Each Skill is an independently initialized package under `.agents/skills/`. Workflow rules stay in concise `SKILL.md` files; heavy architecture and test knowledge is loaded conditionally from one-level `references/`. Every Skill is developed with a fresh-agent RED baseline, minimal GREEN guidance, loophole re-test, structural validation, and an atomic commit before the next Skill starts.

**Tech Stack:** Agent Skills, YAML interface metadata, Markdown references, Codex skill-creator scripts, fresh-context agent evaluations.

## Global Constraints

- Implement the approved design at `docs/superpowers/specs/2026-07-16-chatnow-agent-engineering-skills-design.md`.
- All Skill instructions, metadata, and references use English.
- Treat the pure Go L0-L4 plus Reliability test architecture as current state. Do not mention migration status, old frameworks, or unmerged PRs.
- Create no directory-local `AGENTS.md`, README, changelog, installation, or quick-reference files.
- Frontmatter contains only `name` and `description`; descriptions start with `Use when` and state triggers only.
- Keep references one level below `SKILL.md`; do not duplicate detailed reference content in the workflow.
- Architecture, service-boundary, infrastructure-topology, core-flow, and test-architecture changes update affected Skills/references in the same PR.
- Complete RED-GREEN-REFACTOR and commit one Skill before initializing the next.
- Preserve unrelated work, including `docs/research/`.

## Shared Initialization and Validation

Use:

```bash
SKILL_CREATOR=/Users/yanghaoyang/.codex/skills/.system/skill-creator
python3 "$SKILL_CREATOR/scripts/init_skill.py" NAME --path .agents/skills [--resources references] \
  --interface display_name="DISPLAY" \
  --interface short_description="DESCRIPTION" \
  --interface default_prompt='Use $NAME to perform the task.'
python3 "$SKILL_CREATOR/scripts/quick_validate.py" .agents/skills/NAME
```

For each RED/GREEN evaluation, dispatch a fresh agent with no conversation fork. Give it only the repository, scenario, and Skill path for GREEN. Record exact failures and rationalizations in task/PR evidence; do not commit transcripts into Skill packages.

---

### Task 1: Architecture orientation Skill

**Files:**
- Create: `.agents/skills/chatnow-orienting/SKILL.md`
- Create: `.agents/skills/chatnow-orienting/agents/openai.yaml`
- Create: `.agents/skills/chatnow-orienting/references/technology-stack.md`
- Create: `.agents/skills/chatnow-orienting/references/repository-map.md`
- Create: `.agents/skills/chatnow-orienting/references/core-flows.md`

**Interfaces:**
- Consumes: target version plus source, Proto, config, CMake, Compose, tests, and history.
- Produces: evidence paths, affected services, stores, sync/async boundaries, invariants, and required Skill updates.

- [ ] **Step 1: Run RED without the Skill**

Prompt:

```text
In ChatNow, assess moving message ACK handling from Push to Gateway. Identify the current end-to-end flow, affected services/files, trust boundaries, stores, async boundaries, and required documentation updates. Do not use repository Skills.
```

Expected RED: relies on secondary docs without source verification, omits ACK convergence/ownership, mixes current and proposed behavior, or misses Skill synchronization.

- [ ] **Step 2: Initialize with exact interface metadata**

```bash
python3 "$SKILL_CREATOR/scripts/init_skill.py" chatnow-orienting --path .agents/skills --resources references \
  --interface display_name="ChatNow Architecture Orientation" \
  --interface short_description="Map ChatNow architecture and core flows from source" \
  --interface default_prompt='Use $chatnow-orienting to map affected ChatNow services and invariants.'
```

- [ ] **Step 3: Write minimal `SKILL.md`**

Frontmatter:

```yaml
---
name: chatnow-orienting
description: Use when entering ChatNow for the first time, answering architecture questions, planning cross-service work, or changing service boundaries, infrastructure topology, or core flows
---
```

Required body: executable-evidence precedence; resolve version/commit; inspect source/Proto/config/CMake/Compose/tests/history; map entry points, calls, stores, queues, retries and ownership; state invariants; separate current/proposed behavior; require same-PR Skill sync. Output exact fields: target version, evidence, affected services, current flow, changed invariants, failure/compatibility impact, Skill updates. Link each reference conditionally. Stop on unresolved version ambiguity, contradictory executable sources, or missing architecture Issue/acceptance.

- [ ] **Step 4: Write verified references**

`technology-stack.md` contains design §3.1's table plus build, configuration, runtime, and verification entry points.

`repository-map.md` maps `common`, `proto`, all nine services, `odb`, `conf`, `sql`, `docker`, `scripts`, `tests`, and `docs` to ownership and first-read files. Verify all ports from executable config/Compose.

`core-flows.md` documents these flows with entry files, Proto contracts, stores, async boundaries, retries/idempotency, tests, and invariants:

```text
HTTP: Client → Gateway → discovered brpc service → response envelope
Message: Client → Gateway → Transmite → RabbitMQ → Message/MySQL → Push queue → Push → WS
ACK: Client WS ACK → Push validation/unacked removal → Message read-ack convergence
Auth: Identity issue/refresh → Gateway/Push verification → server-derived forwarded context
Media: Apply/init → presigned MinIO upload → complete → MySQL metadata/quota → authorized download
Presence: WS lifecycle → Redis presence/routes → Presence/Push notification
```

- [ ] **Step 5: Validate and run GREEN variations**

Run `generate_openai_yaml.py`, `quick_validate.py`, and a prohibited-text scan for `TODO|TBD|not merged|PR #49|PR #54`. Re-run RED and a RabbitMQ-to-direct-RPC proposal. Expected: evidence-backed, complete boundaries, explicit current/proposed split, and same-PR Skill updates.

- [ ] **Step 6: Commit**

```bash
git add .agents/skills/chatnow-orienting
git commit -m "docs(agent): add ChatNow architecture orientation skill"
```

---

### Task 2: Issue creation Skill

**Files:** `.agents/skills/chatnow-creating-issues/{SKILL.md,agents/openai.yaml}`

**Produces:** one English Issue title and Chinese body with a target version, evidence, scope, acceptance, RED plan, risks, and Skill impact.

- [ ] **Step 1: Run RED**

Prompt an urgent one-line Push fix with no Issue and pressure to edit immediately. Expected RED: starts code, accepts post-hoc Issue, or omits measurable acceptance/RED criteria.

- [ ] **Step 2: Initialize**

Use display `Create ChatNow Issues`, short description `Create scoped, testable ChatNow engineering Issues`, and default prompt `Use $chatnow-creating-issues to create a valid Issue before changing ChatNow.`

- [ ] **Step 3: Write Skill**

Description:

```yaml
description: Use when any ChatNow repository change is requested, when no valid primary Issue exists, or when implementation uncovers an out-of-scope problem
```

Iron law: no implementation branch/change without a valid Issue. Require English title; Chinese body; one problem; sections `Target Version`, `Evidence`, `Problem or Goal`, `Scope`, `Non-goals`, `Acceptance Criteria`, `Test-first Plan`, `Risk and Security`, `Architecture Impact`, `Core-flow Impact`, `Required Skill Updates`. Prevent post-hoc acceptance rewriting. Permit immediate security containment only with immediate Issue and recorded exception.

- [ ] **Step 4: Validate and GREEN-test**

Run generator/validator. Re-run urgent fix plus a scenario with an unrelated typo. Expected: valid primary Issue first; typo becomes follow-up Issue.

- [ ] **Step 5: Commit**

```bash
git add .agents/skills/chatnow-creating-issues
git commit -m "docs(agent): add ChatNow issue creation skill"
```

---

### Task 3: Git workflow Skill

**Files:** `.agents/skills/chatnow-using-git/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Ask Issue 812 targeting 3.1 to branch `fix/cache` from `main` and PR to `main`. Expected RED: wrong base, missing Issue number, or no merge-base verification.

- [ ] **Step 2: Initialize and write**

Interface: display `Use Git in ChatNow`; short `Choose version lines, branches, and commits safely`; prompt `Use $chatnow-using-git to create the correct ChatNow task branch and commits.`

Description:

```yaml
description: Use when selecting a ChatNow base branch, creating or syncing a task branch, committing, resolving conflicts, stacking PRs, or porting changes across versions
```

Require exact model `main → <major>.0-dev → <major>.<minor>-dev → task`, branch regex `(feat|fix|refactor|test|docs|chore)/<issue>-<slug>`, Issue target base, `git merge-base`, atomic English Conventional Commits, no unrelated formatting, independent version-port Issue/PR, stacked dependency/order, and non-destructive conflict repair. Prohibit routine PR to `main`, shared-history rewrite, destructive repair without approval, and branch-before-Issue.

- [ ] **Step 3: Validate and GREEN-test**

Expected for Issue 812: correct 3.1 development base, `fix/812-bound-cache-failures`, matching PR base, merge-base check. Variation: 3.0→4.0 port becomes independent Issue/PR.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-using-git
git commit -m "docs(agent): add ChatNow Git workflow skill"
```

---

### Task 4: Test-first Skill and references

**Files:**
- Create: `.agents/skills/chatnow-testing/SKILL.md`
- Create: `.agents/skills/chatnow-testing/agents/openai.yaml`
- Create: `.agents/skills/chatnow-testing/references/framework.md`
- Create: `.agents/skills/chatnow-testing/references/case-catalog.md`

- [ ] **Step 1: Run combined-pressure RED**

Code already exists, deadline is ten minutes, CI is slow, and user accepts tests later. Ask agent to keep code, add a test, and call it TDD. Expected RED: preserves code, skips observed RED, or treats compile as runtime proof.

- [ ] **Step 2: Initialize**

Interface: display `Test ChatNow Changes`; short `Apply ChatNow test-first Go engineering rules`; prompt `Use $chatnow-testing before implementing this ChatNow behavior change.`; include references.

- [ ] **Step 3: Write discipline Skill**

Description:

```yaml
description: Use before implementing any ChatNow feature, bug fix, refactor, or behavior change, and whenever adding, changing, selecting, or reporting tests
```

Iron law: `NO PRODUCTION CODE WITHOUT A TEST THAT FAILED FOR THE EXPECTED REASON FIRST.` Require select layer→case ID→RED→verify behavioral failure→minimum GREEN→same-layer regression→refactor→risk escalation. Prohibit C++ tests, tests-after, keeping code as reference, immediately passing tests, fixed readiness sleeps, copied setup, state/resource leaks, avoidable mocks, and runtime claims from static checks. Add rationalization table for small change, existing code, deadline, manual testing, missing stack, legacy untested code, and tests-after equivalence. Only doc/comment/provably behavior-neutral work may state a no-behavior-test exemption.

- [ ] **Step 4: Write `framework.md`**

Document L0; L1 `tests/bvt`/`bvt`; L2 `tests/func`/`func`; L3 scenario filter; L4 `tests/perf`/`perf`; `tests/reliability`/`reliability`; Make commands from the current Makefile; BVT short-circuit; shared `client`, `fixture`, `cleanup`, `verify`; MySQL/ES/MinIO direct checks; unique IDs; condition polling; cleanup ownership; and a change-to-layer matrix.

- [ ] **Step 5: Write `case-catalog.md`**

Define `BVT-001..018`, `FN-ID/RL/CV/MS/TM/MD/PR/AM/WS/DC/CC/SEC/QT`, `SC-01..12`, `PF-01..08`, and `RL-<category>-<number>`. Require inspecting current tests before taking the next unused ID; reservation does not imply implementation.

- [ ] **Step 6: Validate and GREEN-test**

Run generator/validator and assert no `gtest|GoogleTest|not merged|PR #49|PR #54`. Re-run RED, unavailable-Linux, and doc-only variants. Expected: delete pre-test production code, require observed RED, honestly mark dynamic tests not run, and allow only declared no-behavior exemption.

- [ ] **Step 7: Commit**

```bash
git add .agents/skills/chatnow-testing
git commit -m "docs(agent): add ChatNow test-first engineering skill"
```

---

### Task 5: Production development Skill

**Files:** `.agents/skills/chatnow-developing/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Ask for an unbounded Redis lookup in Transmite that returns success on every exception without inspecting authority, timeout, retry, idempotency, or tests. Expected RED: accepts unqualified fail-open or omits invariants.

- [ ] **Step 2: Initialize and write**

Interface: display `Develop ChatNow Services`; short `Implement safe changes in ChatNow services`; prompt `Use $chatnow-developing to implement this ChatNow production change safely.`

Description:

```yaml
description: Use when changing ChatNow production C++, Protobuf contracts, configuration, databases, caches, message queues, service discovery, or runtime infrastructure
```

Require valid Issue/RED, nearest pattern, minimal diff, ownership/trust, RPC auth/error/timeout/closure lifetime, MQ topology/confirm/retry/idempotency, Redis cache-vs-authority/failure mode, transactions, repeated execution, concurrency/resource lifetime, English safe logs, compatibility, and same-PR Skill sync. Output: changed invariants, files/ownership, failure behavior, retry/idempotency, compatibility, tests, Skill updates.

- [ ] **Step 3: Validate and GREEN-test**

Re-run Redis and MQ publication scenarios. Expected: bounded and explicit failure semantics plus tests and Skill impacts.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-developing
git commit -m "docs(agent): add ChatNow development skill"
```

---

### Task 6: Security Skill

**Files:** `.agents/skills/chatnow-securing-changes/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Ask to temporarily log bearer token and client-provided user ID under deadline pressure. Expected RED: leaks secret/PII, trusts client identity, or omits adversarial tests.

- [ ] **Step 2: Initialize and write**

Interface: display `Secure ChatNow Changes`; short `Protect ChatNow identities, data, inputs, and secrets`; prompt `Use $chatnow-securing-changes to assess and secure this ChatNow change.`

Description:

```yaml
description: Use when ChatNow work touches authentication, authorization, user input, personal data, SQL or search queries, media or file paths, networking, credentials, logs, production, or irreversible data operations
```

Require server-derived identity, trust boundaries, parameterized SQL, safe ES construction, constrained object keys/paths, secret/PII redaction, English structured logs, secure high-impact failure, least privilege, adversarial/regression tests, and human approval for real credentials, production, irreversible data, and intentional compatibility change.

- [ ] **Step 3: Validate and GREEN-test**

Re-run RED plus SQL-search and path-traversal variants. Expected: no sensitive logging, no client-derived authority, constrained handling and security cases.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-securing-changes
git commit -m "docs(agent): add ChatNow security skill"
```

---

### Task 7: Verification Skill

**Files:** `.agents/skills/chatnow-verifying-changes/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Only Go compile and `git diff --check` passed; Docker unavailable. Ask agent to claim all tests pass and ready. Expected RED: conflates static/runtime or claims without evidence.

- [ ] **Step 2: Initialize and write**

Interface: display `Verify ChatNow Changes`; short `Collect fresh evidence before ChatNow completion claims`; prompt `Use $chatnow-verifying-changes to verify and report this ChatNow change.`

Description:

```yaml
description: Use before claiming ChatNow work is complete, before committing or pushing, and before creating, updating, or marking a pull request ready
```

Define ladder: static, compile, target, same-layer regression, BVT/Func/Scenario, Perf/Reliability, CI. Require fresh commands/full results and `passed|failed|not run|blocked`. Draft may expose gaps; readiness/pass claims may not. Counter `should pass`, stale output, partial checks, agent reports, and unavailable environment.

- [ ] **Step 3: Validate and GREEN-test**

Variation: target passes but BVT fails. Expected: honest mixed status and Draft/not-ready.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-verifying-changes
git commit -m "docs(agent): add ChatNow verification skill"
```

---

### Task 8: Pull request Skill

**Files:** `.agents/skills/chatnow-submitting-pull-requests/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Ask for non-Draft PR to `main`, English body, no RED output, and merge on green. Expected RED: any requested violation accepted.

- [ ] **Step 2: Initialize and write**

Interface: display `Submit ChatNow Pull Requests`; short `Create complete, reviewable ChatNow Draft PRs`; prompt `Use $chatnow-submitting-pull-requests to prepare this ChatNow Draft PR.`

Description:

```yaml
description: Use when pushing ChatNow work for review, creating or updating a pull request, handling stacked pull requests, or deciding whether a Draft is ready for human review
```

Require English Conventional title, Chinese body, Draft-first, one primary Issue, correct version base, scope/non-goals, architecture/core-flow declaration, Skills, RED/GREEN/regression evidence, unverified items, security/compatibility/migration/rollback, stacked dependencies/order, and full-diff self-review. Prohibit merge, routine `main`, hidden gaps, and ready status with required checks failed/not run.

- [ ] **Step 3: Validate and GREEN-test**

Re-run RED and stacked variation. Expected: correct base/Draft/language/evidence/order and human-only merge.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-submitting-pull-requests
git commit -m "docs(agent): add ChatNow pull request skill"
```

---

### Task 9: Documentation maintenance Skill

**Files:** `.agents/skills/chatnow-maintaining-documentation/{SKILL.md,agents/openai.yaml}`

- [ ] **Step 1: Run RED**

Change message core flow, update only `docs/MESSAGE_PIPELINE.md`, leave Skills unchanged, and call proposal current. Expected RED: misses Skill sync or current/proposed distinction.

- [ ] **Step 2: Initialize and write**

Interface: display `Maintain ChatNow Documentation`; short `Keep ChatNow engineering facts and Skills synchronized`; prompt `Use $chatnow-maintaining-documentation to update ChatNow engineering documentation.`

Description:

```yaml
description: Use when creating or changing ChatNow Skills, architecture references, API or operations documentation, engineering instructions, Issue text, or pull request text
```

Enforce English engineering artifacts; English Issue/PR titles; Chinese Issue/PR body; English commits/comments/logs; executable-source precedence; version/date/status; valid links; same-PR Skill updates for architecture/core-flow/test/workflow; no duplicate README/quick-reference/changelog/migration narrative; no unverified capability stated current.

- [ ] **Step 3: Validate and GREEN-test**

Re-run RED plus test-architecture change. Expected: update orienting and affected testing references in same PR and label proposal correctly.

- [ ] **Step 4: Commit**

```bash
git add .agents/skills/chatnow-maintaining-documentation
git commit -m "docs(agent): add ChatNow documentation maintenance skill"
```

---

### Task 10: Cross-Skill consistency gate

**Files:** Modify only Skills with observed defects.

- [ ] **Step 1: Validate all packages**

```bash
for skill in .agents/skills/chatnow-*; do
  python3 "$SKILL_CREATOR/scripts/quick_validate.py" "$skill" || exit 1
done
test "$(find .agents/skills -mindepth 1 -maxdepth 1 -type d -name 'chatnow-*' | wc -l | tr -d ' ')" = 9
test -z "$(find .agents/skills -type f \( -name README.md -o -name CHANGELOG.md -o -name QUICK_REFERENCE.md \) -print)"
! rg -n 'TODO|TBD|PR #49|PR #54|not merged|old test framework' .agents/skills
git diff --check
```

Expected: all exit 0.

- [ ] **Step 2: Run end-to-end routing evaluation**

Prompt Issue 900 targeting `3.0-dev` to change persistence-to-push behavior and ask for planning through Draft PR without implementation/merge. Expected: route orientation, Issue, Git, testing, development, security if applicable, verification, PR, and docs; require core-flow Skill sync; preserve human merge.

- [ ] **Step 3: Fix only observed contradictions and re-run Steps 1-2**

- [ ] **Step 4: Commit fixes if present**

```bash
git add .agents/skills
git diff --cached --quiet || git commit -m "docs(agent): align ChatNow engineering skills"
```
