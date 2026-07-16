---
name: chatnow-testing
description: Use before implementing any ChatNow feature, bug fix, refactor, or behavior change, and whenever adding, changing, selecting, or reporting tests
---

# Test ChatNow Changes

## Core principle

Use ChatNow's pure-Go behavioral test architecture. Static checks support behavioral evidence; they never replace it.

```
NO PRODUCTION CODE WITHOUT A TEST THAT FAILED FOR THE EXPECTED REASON FIRST.
```

Violating the letter of this rule violates its spirit. If production code exists before its failing test, delete that code. Do not retain, consult, adapt, or retype it as a reference. Start from the test.

## Mandatory sequence

1. Inspect the current executable test surface: `tests/Makefile`, `.github/workflows/ci.yml`, and the current `tests/` tree. Read [references/framework.md](references/framework.md) to choose the minimum sufficient layer.
2. Read [references/case-catalog.md](references/case-catalog.md). Inspect current case IDs, select the namespace, and reserve the next unused ID before implementation.
3. Write the smallest Go test that expresses one required behavior.
4. Run that test and observe RED. Confirm a behavioral assertion fails for the expected missing behavior. Compilation, formatting, vetting, a test discovery error, an unavailable dependency, or an immediately passing test is not RED.
5. Write the minimum production change that makes the test pass.
6. Re-run the target test, then the relevant regression set in the same layer.
7. Refactor only while those tests stay green.
8. Escalate verification according to change risk and the layer matrix. Run every available applicable check; report unavailable Linux/full-stack checks honestly and leave them pending.

If any step is violated, delete production code written out of order and restart at step 1.

## Non-negotiable prohibitions

- Do not add or restore C++ tests. ChatNow behavioral tests are Go tests under the current `tests/` architecture.
- Do not write tests after production code and label the result test-first development.
- Do not keep pre-test production code as a reference, even temporarily.
- Do not accept an immediately passing test as RED; repair the test until it fails for the expected behavioral reason.
- Do not use fixed sleeps for readiness or asynchronous convergence. Poll an observable condition with a deadline.
- Do not copy HTTP, WebSocket, fixture, cleanup, or direct-store setup into a test. Extend the shared package when reuse is needed.
- Do not leak users, rows, Redis keys, search indexes/documents, objects, sockets, goroutines, containers, or volumes. Assign cleanup ownership before creating each resource.
- Do not use mocks when the current full-stack client, fixture, or direct-store verifier can exercise real behavior.
- Do not claim runtime behavior from compilation, formatting, vetting, generated-code success, or static inspection.

## Sole exemption

Documentation-only, comment-only, or provably behavior-neutral mechanical work may state: `No behavior test applies because ...`. Explain the proof and still run relevant static checks. No other work is exempt. If runtime behavior could change, follow the full sequence.

## Rationalizations

| Rationalization | Required response |
|---|---|
| "It is a small change." | Small behavior changes still require an observed failing test. |
| "The code already exists." | Delete it and implement again from the failing test. |
| "The deadline is too close or CI is slow." | Reduce scope, not evidence; run the smallest valid RED locally and report remaining checks. |
| "I tested it manually." | Manual observation is not a repeatable regression test. |
| "The Linux/full stack is missing." | Write and inspect the test, run available checks, and report dynamic tests as not run; never report a pass. |
| "This area has untested code." | Add the new behavior test first; existing gaps do not exempt new work. |
| "Tests after prove the same thing." | Tests-after describe the implementation and never prove the test could detect the missing behavior. |

## Output contract

Report the selected layer and case ID, the exact RED command, the actual observed RED result/output, and why that observed failure is the expected missing behavior. Report the GREEN and same-layer regression commands with results, broader risk checks, cleanup ownership, and every check not run with its reason. For the sole exemption, report the exemption statement and proof instead.

## Red flags: stop and restart

- Production code predates the failing test.
- The test passed first time or failed only to compile/start.
- Evidence says "should pass" or "CI will verify later."
- A fixed sleep, copied setup, leaked state, avoidable mock, or static-only runtime claim appears.
- Pressure is being used to redefine tests-after as test-first.

All red flags require correction before implementation or completion.
