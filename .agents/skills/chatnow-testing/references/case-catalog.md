# ChatNow Test Case Catalog

Case IDs are stable identities, not completion claims. Put the ID in the case comment immediately above a descriptive Go `Test...` or `Benchmark...` function. Keep the ID when renaming or moving the same behavior; allocate a new ID for a distinct behavior.

## Namespaces

| Layer/domain | Namespace |
|---|---|
| BVT | `BVT-001` through `BVT-018` |
| Functional: Identity | `FN-ID-<two digits>` |
| Functional: Relationship | `FN-RL-<two digits>` |
| Functional: Conversation | `FN-CV-<two digits>` |
| Functional: Message | `FN-MS-<two digits>` |
| Functional: Transmite | `FN-TM-<two digits>` |
| Functional: Media | `FN-MD-<two digits>` |
| Functional: Presence | `FN-PR-<two digits>` |
| Functional: Auth middleware | `FN-AM-<two digits>` |
| Functional: WebSocket notification | `FN-WS-<two digits>` |
| Functional: Data consistency | `FN-DC-<two digits>` |
| Functional: Concurrency | `FN-CC-<two digits>` |
| Functional: Security | `FN-SEC-<two digits>` |
| Functional: Quota | `FN-QT-<two digits>` |
| Scenario | `SC-01` through `SC-12` |
| Performance | `PF-01` through `PF-08` |
| Reliability | `RL-<category>-<number>` |

Reliability has an executable Compose gate in `tests/reliability` (`make -C tests test-reliability`). The existing RL-05 category contains circuit recovery and Push persistence cases; new cases use the category-number allocation below. A reserved ID is never evidence of a successful run.

## Allocation procedure

1. Inspect the current tree immediately before allocation. Search test source, comments, documentation, and the diff for the exact namespace; do not rely on this catalog to identify availability.
2. Confirm that the behavior is not already represented under another ID.
3. Format Functional numbers as zero-padded two-digit values `01` through `99`, matching the current executable tests (for example, `FN-ID-08`, `FN-MD-20`, and `FN-DC-07`). Choose the lowest unused number after considering concurrent reservations. If a Functional namespace reaches `99`, stop and update this catalog deliberately instead of silently widening the ID. For Reliability, choose the lowest unused category number after considering concurrent reservations. For bounded BVT, Scenario, and Performance namespaces, use only a free ID within the stated range.
4. Record the reservation in the scoped work before implementation and communicate it to concurrent contributors.
5. Reinspect before commit. Resolve collisions by keeping the earlier reservation and renumbering the later one.

A reservation means only "claimed for coordination." It does not mean the test exists, compiles, ran, passed, covers the behavior, or is ready to merge. Remove an abandoned reservation or make its unimplemented status explicit in the owning work item.

## Naming and comments

Use a descriptive Go identifier such as `TestFN_MS_UpdateReadAck_Idempotent`, `TestScenario_MessageSearchES`, or `BenchmarkSendMessage`. Place a concise English case comment immediately above it. This example is an existing BVT identity, not a new reservation:

```go
// BVT-010 | P0 | A user synchronizes a newly sent message.
func TestBVT_SyncMessages_Success(t *testing.T) {
    // Test body.
}
```

The ID comment is the allocation record in test code. Function names describe behavior and remain compatible with the current layer filters, especially the `TestScenario` filter used by `make -C tests test-scenario`.
