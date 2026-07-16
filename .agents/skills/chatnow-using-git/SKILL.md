---
name: chatnow-using-git
description: Use when selecting a ChatNow base branch, creating or syncing a task branch, committing, resolving conflicts, stacking PRs, or porting changes across versions
---

# Using Git in ChatNow

## Core principle

Select every task branch and PR base from the primary Issue's target version, not from convenience or a user's suggested Git shortcut. `main` is the latest released version, not the routine development base.

```text
main → <major>.0-dev → <major>.<minor>-dev → task
```

## Create a task branch

1. Require a valid primary Issue with a number and one target version before creating the branch. Stop if either is missing or ambiguous.
2. Map the target version directly to its development line: `3.0` uses `3.0-dev`; `3.1` uses `3.1-dev`. Confirm that the line exists remotely and that a minor line descends from its owning `<major>.0-dev` line.
3. Fetch without modifying local work: `git fetch origin <target-base>`.
4. Create the task branch from `origin/<target-base>`. Use a name matching `^(feat|fix|refactor|test|docs|chore)/[0-9]+-[a-z0-9]+(?:-[a-z0-9]+)*$`, which represents `(feat|fix|refactor|test|docs|chore)/<issue>-<slug>`.
5. Immediately verify ancestry; do not substitute a diff or log comparison for `git merge-base`:

```bash
expected_base=$(git rev-parse "origin/$target_base")
actual_base=$(git merge-base HEAD "origin/$target_base")
test "$actual_base" = "$expected_base"
```

For Issue 812 targeting 3.1 and bounding cache failures, create `fix/812-bound-cache-failures` from `origin/3.1-dev`, verify the merge base, and open its PR against `3.1-dev`.

## Commit and publish

- Keep each commit atomic and within Issue scope. Use an English Conventional Commit such as `fix(cache): bound cache failures`.
- Inspect staged content before every commit. Exclude unrelated cleanup, generated files, and formatting. Apply required formatting only to touched code; never reformat unrelated files.
- Re-run the merge-base check before publishing. Push the task branch and set the PR base to the same target development line used to create it.
- Never route a routine feature, fix, refactor, test, documentation, or chore PR to `main`.

## Sync and repair safely

Start with `git status`, preserve uncommitted work, fetch, and inspect the divergence and merge base. Abort a conflicted merge or rebase when resolution is uncertain. Prefer a normal merge for a published or shared branch; rebase only an unpublished private task branch. Never rewrite shared history or force-push it. Never use destructive repair, including hard reset, checkout-based discard, branch deletion, or clean, without explicit human approval.

Resolve conflicts file by file from the Issue contract and target-line behavior. Re-run relevant verification, inspect the resulting diff and history, and confirm the PR base remains the Issue target.

## Ports and stacked work

A change from one version line to another is a new unit of work. For example, porting a 3.0 change to 4.0 requires an independent Issue, a new task branch from `4.0-dev`, its own merge-base verification and commits, and a separate PR to `4.0-dev`. Do not retarget or reuse the 3.0 branch or PR.

For stacked PRs, record each dependency, the required merge order, and the final target version line in every affected PR. Base a dependent branch on its declared predecessor only when the stack requires it; after predecessors merge, safely sync and retarget the dependent PR to the final target line. Do not hide undeclared dependencies in branch ancestry.

## Stop conditions

Stop before mutation when the Issue is absent, the target line is ambiguous or missing, ancestry contradicts the version model, local work could be lost, or repair would require destructive Git. Resolve the contract or obtain explicit approval; do not improvise a base, rewrite history, or create a branch before its Issue.
