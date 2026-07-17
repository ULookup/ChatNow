# PR #57 Review Fixes Design

## Scope

Address the three unresolved requested-change threads on PR #57 without
expanding into the delivery-ACK redesign tracked by issue #58:

1. prevent a failed UserInfo generation CAS from publishing stale data to L1;
2. make per-device Unacked storage idempotent by identity rather than payload;
3. turn RL-05 and PF-09 into real CI gates.

The project is pre-release. The Unacked Redis layout may change without a
compatibility reader or migration.

## UserInfo generation fencing

`UserInfoCache::set_if_generation` returns a three-state result:

- `Committed`: Redis generation matched and L2 was written;
- `Conflict`: Redis was reachable but the generation no longer matched;
- `Unavailable`: no Redis client, an open circuit, or a Redis exception.

Transmite may publish an Identity result to L1 after `Committed`. It must not
publish after `Conflict`. After `Unavailable`, it may publish only to the
existing short-lived L1 fallback so singleflight followers share the successful
Identity response while Redis is down. The current request may return its
Identity result in all three states.

## Unacked Pending Ledger

The two Redis keys have separate responsibilities and share a cluster hash tag:

```text
ZSET im:unack:{uid:device}      member=user_seq, score=next_retry_at
HASH im:unack:idx:{uid:device}  field=user_seq, value=payload_b64
```

Lua scripts atomically maintain both keys:

- push uses `ZADD` and `HSET`; retrying the same `user_seq` replaces its score
  and payload without creating another member;
- peek selects due sequence IDs and returns payloads from the HASH in one
  script; incomplete entries are removed from whichever side remains;
- bump updates scores only for IDs that still have payloads and removes ZSET
  orphans;
- ack always removes the sequence ID from both structures.

There is no parser or migration for the former `user_seq:payload` member format.

## CI gates

RL-05 runs in a dedicated `reliability` job for pull requests and schedules.
PF-09 runs in a dedicated scheduled `perf-cache` job through
`make test-perf-cache-gate`. The Compose Transmite command accepts environment
overrides for user and session rate limits; PF-09 supplies limits above its
generated load while normal Compose defaults remain 600 and 3000 per minute.
Each full-stack job owns setup and `if: always()` teardown.

## Verification

- generation conflict, committed, and unavailable policy tests;
- real Redis Unacked overwrite, due-read, orphan repair, bump, and ACK tests;
- workflow/Compose contract tests proving both targets and rate-limit overrides
  are wired;
- existing Go vet/compile checks and the cache/reliability helper suites.

## Non-goals

- no compatibility with old Unacked Redis data;
- no change to `last_read_seq`, `last_ack_seq`, or issue #58;
- no Redis Streams, new broker, or additional cache tier.
