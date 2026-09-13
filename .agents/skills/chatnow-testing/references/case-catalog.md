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

`RL-WORKER-01` is allocated to `tests/reliability/worker_test.go` for explicit process fencing after worker lease revocation, including container PID 1 exit diagnostics and delayed recovery observation. Allocation is not a test-pass claim.

1. Inspect the current tree immediately before allocation. Search test source, comments, documentation, and the diff for the exact namespace; do not rely on this catalog to identify availability.
2. Confirm that the behavior is not already represented under another ID.
3. Format Functional numbers as zero-padded two-digit values `01` through `99`, matching the current executable tests (for example, `FN-ID-08`, `FN-MD-20`, and `FN-DC-07`). Choose the lowest unused number after considering concurrent reservations. If a Functional namespace reaches `99`, stop and update this catalog deliberately instead of silently widening the ID. For Reliability, choose the lowest unused category number after considering concurrent reservations. For bounded BVT, Scenario, and Performance namespaces, use only a free ID within the stated range.
4. Record the reservation in the scoped work before implementation and communicate it to concurrent contributors.
5. Reinspect before commit. Resolve collisions by keeping the earlier reservation and renumbering the later one.

A reservation means only "claimed for coordination." It does not mean the test exists, compiles, ran, passed, covers the behavior, or is ready to merge. Remove an abandoned reservation or make its unimplemented status explicit in the owning work item.

## Naming and comments

RL-MESSAGE-02 is allocated to `TestRL_TextFixtureWaitsForPersistence` for Issue #104. It observes broker acceptance while Message is stopped, rejects a fixture return without a durable row, and requires convergence after restoring the consumer. Its cleanup joins the fixture and waits for persistence before deleting synthetic state. Allocation is not evidence that historical RPC 9002 or Identity crashes are resolved.

FN-TM-02 is allocated to `TestFN_TM_IdempotentRecordWithoutReadableMessage` for Issue #105. It exercises both existing accepted/persisted Redis records against a real healthy Message lookup with no readable row, requires an unavailable response, and preserves the guard without publishing another message.

RL-MESSAGE-01 is allocated to `TestRL_IdempotentResponseDuringMessageOutage` for Issue #105. It stops Message before or after persistence, observes Transmite directly through the existing internal RPC client, checks both cache formats, and requires complete original results after recovery with one message and one timeline entry per member. Allocation alone is not execution evidence.

RL-REDIS-01 is allocated to `TestRL_RedisCircuitCleanupAfterAssertionFailure` for Issue #102. It injects a failing assertion in an isolated subprocess of RL-05, then requires an already prepared caller to send successfully after cleanup. RL-05 retains Gateway outage assertions while measuring the single Transmite's Open rejections using per-request counters and direct internal RPC. Allocation is not execution evidence.

RL-DISCOVERY-01 is allocated to `TestRL_DiscoveryRecoversAfterIdentityAddressChange` for Issue #97. It covers automatic RPC recovery after changing an isolated Compose Identity endpoint's IPv4 address, without restarting callers. The allocation alone is not passing evidence.

RL-DISCOVERY-02 is allocated to `TestRL_RegistryRecoversAfterLeaseExpiry` for Issue #99. It suspends Identity renewal until the exact etcd registration expires, resumes that same process, and requires the key and authenticated account RPC to recover within 60 seconds. Identity, Gateway and Transmite process identities must remain unchanged. Failure cleanup restores Identity before synthetic data cleanup; allocation alone is not passing evidence.

FN-AM-07 is allocated to `TestFN_AM_ExpiredJWTClassification` in `tests/func/jwt_expiry_test.go` for Issue #28. It covers signed JWT expiration classification and rejection controls through Identity and Gateway. Its synthetic signing fixture is `tests/pkg/fixture/jwt.go`; execution requires the isolated stack's synthetic `CHATNOW_JWT_CONFIG`. The case ID alone is not a passing verification result.

Use a descriptive Go identifier such as `TestFN_MS_UpdateReadAck_Idempotent`, `TestScenario_MessageSearchES`, or `BenchmarkSendMessage`. Place a concise English case comment immediately above it. This example is an existing BVT identity, not a new reservation:

```go
// BVT-010 | P0 | A user synchronizes a newly sent message.
func TestBVT_SyncMessages_Success(t *testing.T) {
    // Test body.
}
```

The ID comment is the allocation record in test code. Function names describe behavior and remain compatible with the current layer filters, especially the `TestScenario` filter used by `make -C tests test-scenario`.
