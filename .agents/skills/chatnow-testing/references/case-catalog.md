# ChatNow Test Case Catalog

Case IDs are stable identities, not completion claims. Put the ID in the case comment immediately above a descriptive Go `Test...` or `Benchmark...` function. Keep the ID when renaming or moving the same behavior; allocate a new ID for a distinct behavior.

## Namespaces

| Layer/domain | Namespace |
|---|---|
| BVT | `BVT-001` through `BVT-018` |
| Functional: Identity | `FN-ID-<three digits>` |
| Functional: Relationship | `FN-RL-<three digits>` |
| Functional: Conversation | `FN-CV-<three digits>` |
| Functional: Message | `FN-MS-<three digits>` |
| Functional: Transmite | `FN-TM-<three digits>` |
| Functional: Media | `FN-MD-<three digits>` |
| Functional: Presence | `FN-PR-<three digits>` |
| Functional: Auth middleware | `FN-AM-<three digits>` |
| Functional: WebSocket notification | `FN-WS-<three digits>` |
| Functional: Data consistency | `FN-DC-<three digits>` |
| Functional: Concurrency | `FN-CC-<three digits>` |
| Functional: Security | `FN-SEC-<three digits>` |
| Functional: Quota | `FN-QT-<three digits>` |
| Scenario | `SC-01` through `SC-12` |
| Performance | `PF-01` through `PF-08` |
| Reliability | `RL-<category>-<number>` |

Reliability is a distinct reserved namespace and layer. The current tree has no executable Reliability suite; reserving an ID never authorizes inventing a directory, target, command, or successful run.

## Allocation procedure

1. Inspect the current tree immediately before allocation. Search test source, comments, documentation, and the diff for the exact namespace; do not rely on this catalog to identify availability.
2. Confirm that the behavior is not already represented under another ID.
3. For an open-ended Functional or Reliability namespace, choose the lowest unused number after considering concurrent reservations. For bounded BVT, Scenario, and Performance namespaces, use only a free ID within the stated range.
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
