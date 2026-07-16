# PR #57 CI Artifacts and Native Regressions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce runnable service artifacts once per workflow and automatically execute the two cache native regressions without weakening the Go full-stack gates.

**Architecture:** A repository-owned, cached native builder feeds one artifact producer job. Consumer jobs download a validated Compose layout. CTest is enabled only for two explicitly scoped cache component regressions.

**Tech Stack:** GitHub Actions, Docker BuildKit, Ubuntu 24.04, CMake/CTest, Bash, Go workflow contract tests, Docker Compose, Redis Cluster.

## Global Constraints

- Build all nine service binaries exactly once per workflow run.
- Do not depend on an unpublished or mutable external builder image.
- Missing native dependencies, binaries, shared libraries, or tests must fail closed; required work may not skip.
- `reliability` and `perf-cache` must depend on and download the same immutable artifact before Compose startup.
- Keep the native-test exception limited to `test_user_info_generation_fence` and `test_unacked_pending_ledger`; do not enable the legacy C++ suite or add gtest.
- RL-05 and PF-09 remain Go full-stack gates using their existing Make targets.
- Every full-stack job ends with one `docker compose down -v` step guarded by `if: always()`.

---

### Task 1: Reproducible native builder and Compose artifact packager

**Files:**
- Create: `docker/ci/Dockerfile`
- Create: `docker/ci/dependencies.lock`
- Create: `scripts/package_compose_artifacts.sh`
- Create: `scripts/validate_compose_artifacts.sh`
- Create: `tests/pkg/contracts/artifacts_test.go`

**Interfaces:**
- Produces: directory `compose-artifacts/<service>/{build,depends}` and `compose-artifacts/MANIFEST.sha256`.
- Services: `conversation gateway identity media message presence push relationship transmite`.

- [ ] Write Go contract tests that fail unless the builder uses a digest/tag lock file, the packager enumerates all nine services, rejects missing binaries and unresolved `ldd` output, and the validator verifies the manifest plus executable/shared-library closure.
- [ ] Run `cd tests && go test ./pkg/contracts -run 'TestComposeArtifact' -count=1` and confirm it fails because the builder and scripts do not exist.
- [ ] Add the Ubuntu 24.04 builder definition. Install distribution dependencies and build non-distribution dependencies at exact immutable revisions from `docker/ci/dependencies.lock`; configure `/usr/local` through `ldconfig`. Do not use `latest`, an unpinned branch, or a pre-existing local image.
- [ ] Implement the packager with `set -euo pipefail`, explicit service enumeration, root-build source paths `build/<service>/<service>_server`, executable checks, `ldd` closure copying, and deterministic `sha256sum` manifest generation.
- [ ] Implement the validator with the same explicit service list, `sha256sum --check`, executable checks, and an isolated `ldd` check for every binary.
- [ ] Run the focused Go contract test and shell syntax checks; expect zero failures.
- [ ] Build the builder image and run CMake plus packaging in it; expect nine binaries and a valid manifest.
- [ ] Commit with message `build(ci): package reproducible service artifacts`.

### Task 2: Register and execute the two native regressions

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `common/test/CMakeLists.txt`
- Modify: `common/test/test_user_info_generation_fence.cc`
- Modify: `common/test/test_unacked_pending_ledger.cc`
- Create: `tests/pkg/contracts/native_regressions_test.go`

**Interfaces:**
- Produces CMake option `CHATNOW_BUILD_CACHE_REGRESSION_TESTS`.
- Produces CTest labels `cache-unit` and `redis-cluster`.
- Consumes environment variable `CHATNOW_REDIS_CLUSTER_SEEDS` for the ledger test.

- [ ] Write contract tests that require both exact targets, both `add_test` registrations, labels, timeouts, the opt-in option, and fail-closed dependency resolution when the option is enabled.
- [ ] Run `cd tests && go test ./pkg/contracts -run 'TestNativeCacheRegression' -count=1`; confirm it fails on missing registrations.
- [ ] Enable CTest at the root and add an OFF-by-default `CHATNOW_BUILD_CACHE_REGRESSION_TESTS` option.
- [ ] Replace the current Redis-header conditional with required target/link discovery inside the enabled option. Register generation as `cache-unit`; register ledger as `redis-cluster`, serial, with an explicit timeout and seed environment.
- [ ] Build with `-DCHATNOW_BUILD_CACHE_REGRESSION_TESTS=ON`, run the generation CTest, and confirm it passes.
- [ ] Start a real Redis Cluster, run the ledger CTest with explicit seeds, and confirm it passes; stop the cluster.
- [ ] Re-run the focused contract tests and commit with message `test(cache): register native atomicity regressions`.

### Task 3: Wire the artifact producer and consumers into CI

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/pkg/contracts/ci_gates_test.go`

**Interfaces:**
- Producer job: `service-artifacts`.
- Artifact name: `compose-service-artifacts`.
- Consumers: `reliability`, `perf-cache`.

- [ ] Extend workflow contract tests to require one producer; BuildKit GHA caching; builder build, CMake build, generation CTest, package, validate, and upload in order; consumer `needs`, download, validate, Compose start, gate, and teardown in order; and execution of the Redis ledger CTest against the stack.
- [ ] Run `cd tests && go test ./pkg/contracts -count=1`; confirm the new assertions fail against the old workflow.
- [ ] Add `service-artifacts` to the workflow using `docker/build-push-action` cache-to/cache-from `type=gha`, then execute the native build and packaging inside that image and upload `compose-artifacts` with `actions/upload-artifact`.
- [ ] Make both consumers depend on the producer, download with `actions/download-artifact`, restore service-context directories, validate before Compose, and run the Redis ledger CTest after Redis Cluster readiness and before the Go gate.
- [ ] Preserve PF-09 rate-limit overrides and strict final teardown behavior.
- [ ] Run the full contract suite, YAML parse, and `docker compose config --quiet`; expect zero failures.
- [ ] Commit with message `ci: distribute service artifacts to cache gates`.

### Task 4: End-to-end verification and PR update

**Files:**
- Modify only if verification exposes a defect in Tasks 1-3.

- [ ] Run `git diff --check` and inspect the complete branch diff.
- [ ] Run `cd tests && PATH="$(go env GOPATH)/bin:$PATH" make proto` followed by `go test ./...`, tagged vet commands, and the PF-09 race helper command.
- [ ] Build the builder from a clean Docker cache or pull-free context, build all services, package, validate, and run the generation test.
- [ ] Start the downloaded-equivalent artifact layout with Compose, wait for services, run the ledger CTest, RL-05, and then tear down.
- [ ] Push the branch and inspect the new GitHub Actions run. Do not claim green if an external failure remains.
- [ ] Fetch review threads again and report which new threads are addressed. Do not reply to or resolve GitHub threads without explicit user authorization.
