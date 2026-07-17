# PR #57 CI Artifacts and Go Regressions Design

## Scope

Address the two review threads added after commit `f633006`:

1. make the RL-05 and PF-09 jobs start a runnable service stack from a clean
   GitHub runner;
2. execute generation-fence and Unacked-ledger business regressions through the
   repository's Go test framework.

The change must not hide a missing native toolchain, silently skip a test, or
duplicate a full service build in every gate job.

## Service artifact pipeline

A single producer job builds the native services in a reproducible Linux
builder and packages the exact directory contract consumed by Compose:

```text
compose-artifacts/
  identity/build/identity_server
  identity/depends/*.so*
  ...
  transmite/build/transmite_server
  transmite/depends/*.so*
```

The producer uses a repository-owned builder definition with pinned dependency
revisions and GitHub Actions layer caching. It performs one root CMake build,
then a packaging script copies `build/<service>/<service>_server` into each
service build context and obtains the non-system shared-library closure from
`ldd`. Packaging fails if any of the nine binaries is missing or if `ldd`
reports an unresolved dependency.

The producer uploads one immutable artifact for the workflow run. The
`reliability` and `perf-cache` consumers declare `needs: service-artifacts`,
download it before `docker compose up`, validate its manifest, and then run the
existing Go gates. A failed producer blocks consumers instead of producing a
misleading integration-test failure.

## Go regression coverage

The unified test architecture is authoritative: tests are Go, exercise
HTTP/protobuf or WebSocket service boundaries, and run through targets in
`tests/Makefile`. This PR does not introduce CTest or retain temporary C++
regression executables.

The Unacked regression calls the real Push service twice with the same
`user_seq` and different notification payloads after establishing an online
device route. It verifies through Redis Cluster that the ZSET retains one
stable identity and the HASH contains the second payload, then sends a real
WebSocket ACK and verifies both indexes are removed.

The UserInfo regression remains a full-stack cache-consistency scenario. It
invalidates shared cache state through UpdateProfile, drives the Transmite
lookup path, and verifies that the repopulated Redis value contains the latest
profile after the documented bounded process-local L1 lifetime. The test
asserts externally observable freshness; it does not add a production timing
hook solely to force an internal CAS interleaving.

RL-05 and PF-09 remain the authoritative system gates.

## Contract enforcement

The existing Go workflow contract tests will require:

- exactly one service-artifact producer;
- build, package, validate, and upload steps in order;
- both gate jobs to depend on the producer and download/validate before
  Compose startup;
- the Go cache and Unacked regressions to execute through an existing Make
  target against the stack;
- no `continue-on-error`, conditional bypass, or skip-capable dependency
  detection on required steps;
- teardown to remain the unique final step with `if: always()`.

## Failure behavior

- Missing compiler dependency: builder construction or CMake configuration
  fails.
- Missing binary: packaging fails before upload.
- Unresolved shared library: artifact validation fails before upload and again
  after download.
- Redis Cluster unavailable: the Go Unacked regression fails; it is never
  reported as skipped.
- Gate failure: job fails and teardown still runs.

## Non-goals

- no refactor of all nine runtime Dockerfiles into multi-stage builds;
- no externally managed or mutable CI image dependency;
- no CTest or revival of the old C++ test suite;
- no change to cache, ACK, or message-watermark business semantics.
