---
name: chatnow-maintaining-documentation
description: Use when creating or changing ChatNow Skills, architecture references, API or operations documentation, engineering instructions, Issue text, or pull request text
---

# Maintain ChatNow Documentation

## Core principle

Keep one verified account of each engineering fact. Derive current-state claims from executable evidence, label proposals as proposals, and synchronize every affected Skill and reference in the same PR.

## Evidence and labeling

Use evidence in this order:

1. Production source and Protobuf contracts.
2. Executable configuration, CMake, Docker Compose, tests, and CI workflows.
3. Git history for version-specific intent.
4. README files, plans, specifications, roadmaps, changelogs, and other prose as secondary context only.

Verify secondary claims against higher-precedence evidence. Cite repository paths and symbols or lines for material facts. Do not state an unverified, planned, partially implemented, unavailable, or historical capability as current.

For an engineering reference or proposal, record the applicable version line, verification or proposal date, and one explicit status: `Current`, `Proposed`, `Historical`, `Deprecated`, or `Unverified`. Keep current and proposed behavior in separate sections. A date or confident tone does not turn a proposal into current behavior.

## Mandatory workflow

1. Resolve the primary Issue, target version line, current commit, audience, and canonical document for the subject.
2. Inspect the executable evidence that owns every changed claim. Record contradictions and unknowns instead of resolving them by assumption.
3. Update the canonical artifact in English. Preserve existing compatibility text unless the Issue explicitly changes the contract.
4. Compute synchronization from the substance of the change, not the file extension:

| Changed subject | Required same-PR updates |
|---|---|
| Architecture, service boundary, infrastructure topology, or core flow | `chatnow-orienting` and every affected reference under `.agents/skills/chatnow-orienting/references/` |
| Test architecture, layers, tags, commands, fixtures, case IDs, or test workflow | `chatnow-orienting` where architecture context changes, plus affected `chatnow-testing` instructions and references |
| Mandatory engineering workflow or policy | Every Skill and template that teaches or enforces that workflow |
| Skill trigger or interface | The Skill plus matching `agents/openai.yaml` metadata |

A follow-up Issue is not a substitute for these same-PR updates. Name the exact affected paths.

5. Check every relative link, anchor, repository path, and command against the target commit. Remove stale links; do not link to a path or capability that does not exist.
6. Review the full PR diff for contradictory status labels, duplicated facts, missing Skill synchronization, language violations, and claims unsupported by executable evidence.

## Language contract

- Write Skills, engineering references, API and operations documentation, automation, identifiers, new code comments, and new log messages in English.
- Write Issue and PR titles in English. Write authored Issue and PR body prose in Chinese; preserve required English headings, field names, code, commands, and machine markers.
- Write commit messages in English Conventional Commits.
- Do not rewrite existing Chinese comments merely for translation without an Issue-local reason.

## Canonical-artifact rule

Extend the existing canonical document or Skill. Do not create a duplicate README, quick-reference, changelog, installation guide, migration narrative, process-history file, or stale status snapshot to explain the same facts. Active rollout or compatibility instructions belong in the canonical change document and must carry target version, date, and status.

## Output contract

Report:

1. target version line, commit, document status, and date;
2. executable evidence inspected and unresolved contradictions;
3. canonical files changed;
4. exact same-PR Skill, reference, and template updates, or `None` with evidence that no governed subject changed;
5. language and link-integrity checks performed;
6. unverified claims retained as explicitly `Unverified` or removed.

## Stop conditions

Stop and resolve the contract before claiming documentation current when the target version is ambiguous, executable sources conflict, a material claim is unverified, a proposal is presented as implemented, links are invalid, or required same-PR Skill updates are excluded.

## Common mistakes

| Mistake | Required correction |
|---|---|
| "Only a Markdown file changed." | Classify the changed engineering fact and synchronize affected Skills. |
| "The proposal is approved, so call it current." | Keep it `Proposed` until executable evidence implements it. |
| "The README already says so." | Verify against source, contracts, executable config, tests, and CI. |
| "Update the Skill later." | Include exact affected Skill/reference paths in this PR. |
| "Add a quick guide for visibility." | Update the canonical artifact and link to it; do not create parallel truth. |
| "The link looks right." | Resolve and check the actual target and anchor at the target commit. |
