# PR #57 CI Artifacts and Native Regressions Design

## Scope

Address the two review threads added after commit `f633006`:

1. make the RL-05 and PF-09 jobs start a runnable service stack from a clean
   GitHub runner;
2. execute the generation-fence and Unacked-ledger regressions automatically.

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

## Native regression exception

The unified test architecture remains Go black-box by default. Two narrow L0
native component regressions are an explicit exception because the public
service boundary cannot deterministically create their internal atomicity
conditions without production test hooks:

- UserInfo generation CAS conflict must not publish stale bytes into L1;
- retrying one Unacked `user_seq` with a different payload must keep one
  identity and ACK must remove both indexes.

The exception does not introduce gtest or a general C++ test suite. CMake
exposes `CHATNOW_BUILD_CACHE_REGRESSION_TESTS`, disabled by default and enabled
by CI. When enabled, dependencies are required rather than detected with a
skip-capable probe. Both executables are registered with CTest labels. The
generation test runs in the producer. The ledger test runs against the real
Redis Cluster in a Compose network with an explicit seed and a timeout.

The Go RL-05 and PF-09 suites remain the authoritative business and system
gates; native tests only cover atomicity that cannot be made deterministic at
the external API.

## Contract enforcement

The existing Go workflow contract tests will require:

- exactly one service-artifact producer;
- build, package, validate, and upload steps in order;
- both gate jobs to depend on the producer and download/validate before
  Compose startup;
- the producer to enable and execute the generation native regression;
- the Redis-backed ledger regression to execute against the stack;
- no `continue-on-error`, conditional bypass, or skip-capable dependency
  detection on required steps;
- teardown to remain the unique final step with `if: always()`.

## Failure behavior

- Missing compiler dependency: builder construction or CMake configuration
  fails.
- Missing binary: packaging fails before upload.
- Unresolved shared library: artifact validation fails before upload and again
  after download.
- Redis Cluster unavailable: ledger CTest fails; it is never reported as
  skipped.
- Gate failure: job fails and teardown still runs.

## Non-goals

- no refactor of all nine runtime Dockerfiles into multi-stage builds;
- no externally managed or mutable CI image dependency;
- no general revival of the old C++ gtest suite;
- no change to cache, ACK, or message-watermark business semantics.
