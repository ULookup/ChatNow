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
