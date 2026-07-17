---
name: chatnow-verifying-changes
description: Use before claiming ChatNow work is complete, before committing or pushing, and before creating, updating, or marking a pull request ready
---

# Verify ChatNow Changes

## Core principle

Make every completion, pass, and readiness claim from fresh evidence for the current change. Static evidence does not prove runtime behavior, and an expected result is not an observed result.

## Required inputs

Before running checks, identify:

- the current commit, working-tree diff, and files changed;
- the Issue acceptance criteria and behavior affected;
- the target Go test and same-layer regression scope selected with `chatnow-testing`;
- the environment and unavailable dependencies;
- the requested decision: local status, commit/push, Draft PR, or ready PR.

Evidence is fresh only when collected in the current workspace after the last relevant code, test, configuration, generated-file, or dependency change. Output from an earlier commit, another worktree, a prior CI run, or another agent is context to recheck, not verification.

## Evidence ladder

Inspect the current `tests/Makefile`, `.github/workflows/ci.yml`, and test tree before choosing exact commands. Use `chatnow-testing` for the authoritative layer and case selection. Run applicable checks in this order:

| Step | Evidence | Requirement |
|---|---|---|
| 1 | Static checks | Run checks applicable to the diff, including formatting, generated-file checks, vetting, policy checks, and `git diff --check`. |
| 2 | Compilation | Compile every affected C++ or Go build surface; compilation alone is not a test pass. |
| 3 | Target test | Run the smallest exact test that proves the changed behavior. |
| 4 | Same-layer regression | Run the affected package or complete layer after the target passes. |
| 5 | BVT, Functional, Scenario | Run each applicable correctness gate; record them separately rather than collapsing them into "tests." |
| 6 | Performance, Reliability | Run the risk-relevant layer. Reliability is reserved in the current architecture: inspect for an executable surface, but never invent a directory, tag command, Make target, or successful run. |
| 7 | CI | Record the current commit's actual workflow and job results; an old green run is not evidence for the current commit. |

Do not skip a lower rung because a higher rung passed. A broad check does not replace the exact target result, and a target pass does not replace regressions.

## Record each check

For every rung and separately named gate, record all fields:

```text
Check:
Required: yes | no
Status: passed | failed | not run | blocked
Command:
Full result:
Freshness:
Reason or next action:
```

Use statuses literally:

- `passed`: the exact command ran now to completion with a successful exit and its full result was inspected.
- `failed`: the command ran and returned a failing assertion, unsuccessful exit, or required-check error.
- `not run`: it was not executed or does not apply; state which and why. Never attach a predicted outcome.
- `blocked`: it is required and applicable, but an unavailable tool, service, credential, platform, or environment prevented execution; name the blocker and the exact command that remains.

For CI, include the commit SHA, workflow/run identity, job results, and failing or pending details. For an unavailable reserved Reliability layer, leave `Command` as `No executable command exists in the current repository`, set `not run`, and explain whether its absence is a readiness gap for this change.

Do not replace full output with "clean," "looks good," a success count, an agent summary, or "see above." Preserve the exact command, exit status, and complete stdout/stderr in the verification evidence or an identified durable log artifact.

## Decide the claim

Separate the action from the evidence verdict:

- **Draft / not ready:** allowed when checks are `failed`, `blocked`, or required-but-`not run`, provided every gap and next action is explicit.
- **Ready / complete / all applicable tests pass:** allowed only when every applicable required check for that claim is fresh and `passed`. A required CI job that is pending, blocked, stale, or absent keeps a PR not ready.
- **Commit or push:** report the matrix honestly; do not convert the action into a completion or readiness claim. Use Draft when pushing is necessary to obtain CI or unavailable dynamic evidence.

Any failure dominates. For example, a passed target with failed BVT is a mixed result: report `Target: passed`, `BVT: failed`, and `Verdict: Draft / not ready`. Never summarize that state as verified, passing, or ready.

## Rationalizations to reject

| Claim | Required response |
|---|---|
| "It should pass." | Mark the check `not run`; predictions are not evidence. |
| "It passed earlier." | Re-run after the latest relevant change or mark stale evidence `not run`. |
| "Compile and diff checks passed." | Report those rungs only; do not infer target or runtime behavior. |
| "Another agent says it passed." | Obtain the exact fresh command and full result, then independently validate their scope and workspace identity. |
| "Docker or the full stack is unavailable." | Mark applicable dynamic checks `blocked`, name the blocker, and keep the result Draft / not ready. |
| "The target test passed, so all tests pass." | Run and report same-layer and broader applicable regressions separately. |
| "Prior CI was green." | Match CI evidence to the current commit or report current CI `not run`. |

## Output contract

Report the current commit/diff identity, requested decision, and one complete check record for every ladder rung. Then report:

```text
Passed:
Failed:
Not run:
Blocked:
Verdict: Draft / not ready | ready for the stated action
Next action:
```

Never use "all tests pass," "complete," "verified," or "ready" when the evidence table contains an applicable failure or unresolved required gap.
