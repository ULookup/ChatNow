# CI runtime repair for Issue #88

Target version: `3.0-dev`
Status: Current
Reviewed: 2026-09-13

Draft PR #89. The owner approved extending #88 on 2026-09-13 to cover defects exposed by the restored runtime gates. This work does not close the larger follow-up Issues or change multi-device coexistence and caller-only message deletion.

## Reproduced failures and repairs

| Failure | Cause and repair | Runtime regression |
|---|---|---|
| Agent Policy | The PR Target Version section mixed the branch with prose; it now contains exactly `3.0-dev`. | Local validator RED/GREEN and GitHub Agent Policy run 34715937467 |
| Conversation search | An exact membership filter targeted an analyzed string; use the existing dynamic mapping's `member_ids.keyword`, consistently with other identifier filters. | `TestSearchConversations_Success` and non-member search cases |
| Multipart HTTP failures | Four RPCs had no Gateway routes. Restore JWT-protected routes. Fixtures must use an allowed MIME and valid S3 part sizes. | FN-MD-04 through FN-MD-10; SC-05; FN-MD-22 |
| Multipart PUT HTTP 403 | Upload ID and part number were appended after SigV4 signing. Sign the complete query. | FN-MD-07 downloads and compares the full object; FN-MD-21 rejects a changed part number |
| Avatar cannot be downloaded | Identity fabricated a URL from a file ID, although storage keys use a content hash. Resolve Media's canonical public object URL through discovery. | FN-ID-08 downloads the uploaded bytes and re-reads the profile |
| Missing online notifications | Friend application, accepted application and conversation creation did not call Push. Send bounded best-effort notifications after domain persistence, without broadcasting the creator's personal member state. | FN-WS-02/03/04 |
| Reconnect loses immediate delivery | Push cached an empty route and did not invalidate on authentication. Fence route fills and invalidation with the same per-user lock, and avoid negative route caching. | SC-03, FN-WS-01/05/07 |
| MQ trace absent | Release builds removed the log-context key initialization because it was inside `assert`. Initialize the key unconditionally and abort on allocation failure. Transmite also copies its authenticated trace explicitly into the MQ header. | FN-WS-08 |
| Unread count remains zero | Message persistence never advanced the Conversation watermark. Commit it atomically with message/timeline rows and prevent stale metadata writes from decreasing it. | SC-09 and FN-CC-02 |
| Sending to a dismissed group succeeds | Cold member-cache fills did not validate conversation status. Check the authoritative status before serving membership. | Dismissed-conversation send case and membership regressions |
| Invalid audio reports success | The placeholder ASR handler acknowledged bytes without processing them. Reject malformed PCM16 and return unavailable for valid input until a backend is integrated. | FN-MD-17/18/23; replaces the contradictory fake-audio success assertion |
| Reliability timeout / no rate limiting | A slow serial 650-request stream refilled the default token bucket. The dedicated test stack uses limits 8/40 and a bounded 32-request burst; defaults remain 600/3000. | RL-05 circuit/recovery |
| Redis circuit never opens | Pinned Redis++ wraps exhausted shard refresh attempts in a base `Error`. Classify that exact availability error, while retaining separate command-error handling. | RL-05 circuit counters and fast failure |
| Cache outage masks sequencing failure | An unavailable member snapshot returned before consulting the authoritative service. Mark its version unknown and use an uncached Conversation lookup without publishing an unfenced cache fill. | RL-05 sequencing failure and membership regressions |
| Fast-failure sample takes a recovery probe | Three unrelated outage RPCs crossed the one-second Open deadline before measurement. Measure Open immediately, then exercise those RPCs; retain the 50 ms limit and recovery assertions. | RL-05 fast-failure timing |
| Push reliability socket missing | The test authenticated using a device ID different from its JWT. Use the issued ID and check only the unpersisted outage marker. | RL-05 Push persistence/requeue |
| Push records requeue but loses the message | AMQP-CPP `reject` takes bit flags; boolean `true` does not set `AMQP::requeue`. Use the explicit flag for retry actions and exceptions in both consumer overloads. | RL-05 durable Unacked and post-recovery delivery |

## Compatibility and operational boundaries

`FileInfo.public_url` is additive field 6. It is populated only for committed public-bucket objects; private objects keep an empty value. Identity returns an empty avatar URL when Media resolution is unavailable. The Media public prefix is authoritative. The HTTP routes, multipart request/response shapes, additive field and ASR failure semantics are recorded in `docs/api/openapi-media.yaml`.

Message gains only `SELECT, UPDATE` on `conversation` through the existing idempotent principal convergence script. No table migration or production data repair is performed. Deployments must converge this grant before starting the new Message binary.

The existing Elasticsearch index-creation helper still produces dynamic mappings. This repair follows that deployed field contract; it does not recreate or migrate indexes. A future explicit-mapping migration must update the exact-match queries together.

Notifications remain online best effort. The existing domain query APIs are authoritative after lost notifications; this patch does not introduce a business-notification outbox or claim durable notification delivery.

RL-05 pauses all six Redis processes while preserving container DNS and networking. This injects an unresponsive Redis service and tests socket timeouts, circuit opening, recovery and durable Unacked ordering. Stopping the containers also withdraws their DNS records and exposed a separate resolver/restart limitation in the local RED run; pause-based results do not establish stop/recreate recovery or resolve #73.

## Research

- [AWS SigV4 query authentication](https://docs.aws.amazon.com/AmazonS3/latest/developerguide/sigv4-query-string-auth.html): the canonical query includes request query parameters before signing.
- [AWS C++ S3Client API](https://docs.aws.amazon.com/sdk-for-cpp/latest/api/aws-cpp-sdk-s3/html/class_aws_1_1_s3_1_1_s3_client.html): endpoint resolution and presigning APIs.
- [Elasticsearch dynamic field mapping](https://www.elastic.co/docs/manage-data/data-store/mapping/dynamic-field-mapping): dynamically mapped strings provide a keyword subfield for exact matching.
- [Pinned Redis++ shard refresh implementation](https://github.com/sewenew/redis-plus-plus/blob/a63ac43bf192772910b52e27cd2b42a6098a0071/src/sw/redis++/shards_pool.cpp): exhausted topology refresh attempts produce the base error `Failed to update shards info`.
- [Docker container pause](https://docs.docker.com/reference/cli/docker/container/pause/): Linux containers use the freezer mechanism to suspend processes; RL-05 uses this fault without removing the container network endpoint.
- [Pinned AMQP-CPP flags](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/ca49382bfc5bc165dfb7988b891bb010a939a786/src/flags.cpp) and [channel API](https://github.com/CopernicaMarketingSoftware/AMQP-CPP/blob/ca49382bfc5bc165dfb7988b891bb010a939a786/include/amqpcpp/channel.h): `reject` requires the `requeue` bit for redelivery; boolean `true` selects no supported rejection flag.

## Verification record

Gate results are commit-specific. Consult [PR #89](https://github.com/ULookup/ChatNow/pull/89)'s GREEN Evidence, Regression Verification and current-head checks for execution results. Full commands, exits and logs are retained in the task audit directory `pr89-ci-followup`. Historical passes do not verify later edits, and current-head CI must pass before a completion claim. The PR remains Draft pending human review.
