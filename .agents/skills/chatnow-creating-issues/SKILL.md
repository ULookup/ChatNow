---
name: chatnow-creating-issues
description: Use when any ChatNow repository change is requested, when no valid primary Issue exists, or when implementation uncovers an out-of-scope problem
---

# Creating ChatNow Issues

## Core principle

```
NO IMPLEMENTATION BRANCH OR REPOSITORY CHANGE WITHOUT A VALID PRIMARY ISSUE.
```

Urgency, a one-line change, or permission to document later does not weaken this rule. Continue only read-only investigation until the Issue exists and has a number or URL.

## Create the Issue first

1. Verify the request against source, logs, tests, or observed behavior. Do not turn an unverified suspected fix into the requirement.
2. Select the target version line. Stop if multiple targets remain valid after checking available Issue, milestone, and repository evidence.
3. Define exactly one primary problem or deliverable. Split independent work into separate Issues.
4. Write an English title and a body whose prose is Chinese. Use every heading below exactly once and in this order.
5. Validate the Issue contract, create the Issue, and record its number or URL before creating a task branch or changing the repository.

## Required body contract

```markdown
## Target Version

## Evidence

## Problem or Goal

## Scope

## Non-goals

## Acceptance Criteria

## Test-first Plan

## Risk and Security

## Architecture Impact

## Core-flow Impact

## Required Skill Updates
```

Fill the sections as follows:

| Section | Required content |
|---|---|
| `Target Version` | One existing development version line and the evidence for selecting it. |
| `Evidence` | Specific source paths, logs, failing tests, or reproducible behavior. Mark unknown facts as unknown; never invent evidence. |
| `Problem or Goal` | Current behavior or need, affected users or systems, and the observable desired outcome without assuming an implementation. |
| `Scope` | Files, services, behavior, and deliverables included in this Issue. |
| `Non-goals` | Closely related work explicitly excluded. |
| `Acceptance Criteria` | Observable, measurable checkboxes, including relevant boundaries and failure behavior. Do not encode an unverified implementation as acceptance. |
| `Test-first Plan` | The smallest RED case, its command and expected failure reason, the minimum GREEN behavior, and relevant regression coverage. |
| `Risk and Security` | Security, privacy, data, compatibility, migration, operational, and rollback risks, or an evidence-based `None`. |
| `Architecture Impact` | `Yes` or `No` plus reasoning about service boundaries and infrastructure topology. |
| `Core-flow Impact` | `Yes` or `No` plus reasoning about communication, auth, persistence, indexing, caching, sequencing, idempotency, retry, Outbox, Push, or ACK behavior. |
| `Required Skill Updates` | Exact affected ChatNow Skill/reference paths, or `None` with reasoning. Any architecture, core-flow, or test-architecture change requires relevant Skill updates in the same PR; a follow-up Issue is not a substitute. |

## Preserve the contract

Update evidence as investigation improves it. Never rewrite acceptance criteria after implementation merely to fit the produced code. If a genuine requirement change is approved, record the reason and changed requirement in the Issue before continuing, then restart planning and RED against the revised contract.

An unrelated defect, typo, cleanup, or cross-version port is not current scope. Create a follow-up Issue and keep it out of the current branch and PR. One PR resolves one primary Issue.

## Security containment exception

Immediate containment may precede the normal Issue-first order only when delaying action materially increases security harm. Create the Issue immediately, and record the exception, the harm caused by delay, containment actions, timestamps, evidence, and remaining follow-up. This exception permits only the minimum containment; it does not permit a post-hoc ordinary Issue or hidden scope.

## Stop conditions

Stop implementation and continue only read-only research when:

- GitHub is unavailable and no valid Issue exists;
- the target version remains ambiguous;
- the Issue combines multiple primary problems;
- evidence is missing or contradicts the proposed goal;
- acceptance or the RED plan is not measurable;
- security or data risk cannot be bounded safely.

## Common rationalizations

| Rationalization | Required response |
|---|---|
| "It is only one line." | Create and validate the Issue first. Size does not remove risk or traceability. |
| "Open the Issue after the fix." | Stop. Post-hoc documentation cannot authorize implementation. |
| "The deadline is too close for RED." | Define measurable acceptance and an observed RED plan before implementation. |
| "We already know the operator change." | Record verified behavior and outcome; do not make an assumed patch the contract. |
| "Fix this unrelated typo while here." | Create a follow-up Issue and exclude it from the current branch and PR. |
| "Update the architecture Skill later." | Update every relevant Skill/reference in the same architecture or core-flow PR. |
