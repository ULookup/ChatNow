---
name: chatnow-submitting-pull-requests
description: Use when pushing ChatNow work for review, creating or updating a pull request, handling stacked pull requests, or deciding whether a Draft is ready for human review
---

# Submit ChatNow Pull Requests

## Core principle

Create every ChatNow PR as a complete, honest Draft against the primary Issue's target version line. A pushed branch and a green check do not grant merge authority: merge remains human-only.

## Establish the PR contract

1. Require one valid primary Issue. Use exactly one closing declaration, `Closes #N`; references to follow-up or dependency Issues must not close them.
2. Read the Issue's target version and use its development line as the PR base. Confirm the head branch follows `chatnow-using-git` and verify its merge base. A routine task PR never targets `main`.
3. Use an English Conventional Commit-style title: `<type>: <English summary>` or `<type>(<scope>): <English summary>`, where type is `feat`, `fix`, `refactor`, `test`, `docs`, or `chore`. Use a Chinese body for authored prose; paths, commands, identifiers, evidence, and machine markers may remain English.
4. Create the PR as Draft. Draft status exposes incomplete evidence; it does not hide it.
5. Use `chatnow-verifying-changes` to collect fresh evidence and decide readiness. Do not summarize a failed, blocked, stale, pending, or required-but-not-run check as passing.
6. Inspect the complete diff from the selected base through `HEAD`, not only the last commit or files remembered from implementation.

## Quick reference

| Decision | Required result |
|---|---|
| Initial state | Draft |
| Primary Issue | Exactly one `Closes #N` |
| Base | Issue target development line; not routine `main` |
| Title and body | English Conventional title; Chinese body prose |
| Evidence gap | Declare it and keep the PR not ready |
| Merge | Authorized human only; no auto-merge |

## Required body contract

Use every heading below exactly once. Fill every field; use `None` only with a Chinese evidence-based reason.

```markdown
## Primary Issue
Closes #N

## Target Version
<Issue target version and exact PR base>

## Scope
<included behavior, services, files, and deliverables>

## Non-goals
<closely related work excluded from this PR>

## Architecture Impact
Yes | No — <reason about service boundaries or infrastructure topology>

## Core-flow Impact
Yes | No — <reason about communication, auth, persistence, indexing, caching,
sequencing, idempotency, retry, Outbox, Push, or ACK behavior>

## Updated Skills
<exact affected .agents/skills/... paths, or None with reason>

## RED Evidence
<exact command, observed failure, and why it proved the missing behavior>

## GREEN Evidence
<exact command and fresh full result>

## Regression Verification
<exact commands and fresh full results>

## Security and Compatibility
Security: <impact or reason for none>
Compatibility: <impact or reason for none>
Migration: <required migration, sequencing, approval, and status, or reason for none>

## Unverified Items
<each not-run, blocked, failed, pending, or stale check and its next action>

## Rollback Plan
<safe rollback trigger, procedure, data implications, and owner/approval boundary>

## Stacked PR Dependencies
Dependency: <PR/branch or None>
Final target version: <development line>
Merge order: <ordered PR list or None>
After predecessor merge: <sync and retarget action or None>

## Full-diff Self-review
Base and range: <base...HEAD>
Verdict: <scope, correctness, tests, security, compatibility, generated files,
documentation/Skill synchronization, and unrelated-change findings>
```

The evidence sections must report observed output. If valid RED evidence was not captured, say so in `RED Evidence`, keep it in `Unverified Items`, and do not invent or omit it. For documentation-only, comment-only, or provably behavior-neutral mechanical work, state the `chatnow-testing` exemption and its proof instead of fabricating RED/GREEN behavior evidence.

## Declare architecture, Skills, and the full diff

Name exact affected Skill or reference paths; do not write only `docs updated`, `N/A`, or a category name. Any architecture, service-boundary, infrastructure-topology, core-flow, test-architecture, or mandatory-workflow change requires all affected Skills/references in this same PR. A follow-up Issue is not a substitute.

Before creating or updating the PR, inspect at least:

```bash
git status --short
git merge-base HEAD "origin/$base"
git diff --stat "origin/$base"...HEAD
git diff --check "origin/$base"...HEAD
git diff "origin/$base"...HEAD
git log --oneline "origin/$base"..HEAD
```

Record the range and verdict in `Full-diff Self-review`. Resolve or explicitly remove unrelated changes; check Issue scope, acceptance, generated artifacts, sensitive data, compatibility, test evidence, and required documentation/Skill synchronization.

For a stack, declare every predecessor in every affected PR, the final version-line base, and one unambiguous merge order. A dependent PR may temporarily target its predecessor branch; after that predecessor merges, safely sync it and retarget it to the declared final version line. Never hide dependency in ancestry or leave the order unresolved.

## Readiness and authority

- Keep the PR Draft while any applicable required check is failed, blocked, pending, stale, or not run, or while Issue/base/scope/Skill synchronization remains unresolved.
- Mark ready only after the current-head verification matrix shows every applicable required check passed and the complete-diff self-review has no unresolved finding.
- Never merge a PR, enable auto-merge, schedule merge-on-green, or describe green CI as merge approval. Request human review and leave the merge action to an authorized human.

## Stop conditions

Stop before PR creation or readiness when the primary Issue is absent or ambiguous, the target base conflicts with it, a routine PR targets `main`, body language or required sections are invalid, evidence gaps are hidden, architecture/core-flow Skill updates are missing, the stack order is unresolved, the full diff has not been reviewed, or any required check is not freshly passed. A Draft may be created only when every known gap is explicit and the base and Issue are valid.

## Common rationalizations

| Rationalization | Required response |
|---|---|
| "Open it ready to save a click." | Create a Draft; readiness is an evidence verdict. |
| "The user asked for `main`." | Use the Issue target line; reject a routine `main` PR. |
| "No RED was saved, so omit it." | Declare the missing evidence and keep the PR not ready. |
| "Green CI can merge automatically." | Never enable auto-merge; merge is human-only. |
| "Migration is obvious or not relevant." | Fill the explicit Migration field with impact and evidence. |
| "I reviewed the files I changed." | Inspect and record a verdict for the complete base-to-HEAD diff. |
| "The dependency is visible in Git." | Declare every dependency, final target, merge order, and retarget action. |
