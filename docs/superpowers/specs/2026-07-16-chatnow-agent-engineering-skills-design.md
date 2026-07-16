# ChatNow Agent Engineering Skills Design

> Status: Approved for implementation
> Date: 2026-07-16
> Repository: `ULookup/ChatNow`
> Architecture baseline: ChatNow 3.0 development line
> Audience: Coding agents only

## 1. Purpose

ChatNow will manage its engineering rules as repository-local, task-triggered Agent Skills rather than as a conventional contributor handbook. The system must give coding agents enough project-specific context and procedural guidance to autonomously take a valid Issue through implementation, verification, commit, push, and Draft PR creation while preserving explicit human approval boundaries.

The design has four parts:

1. A short root `AGENTS.md` containing project invariants, Skill routing, rule precedence, and approval boundaries.
2. Nine repository-local Skills under `.agents/skills/` containing focused workflows and references.
3. Structured GitHub Issue Forms and a PR template that make the required context machine-readable.
4. A tested policy CLI and GitHub Actions workflow that enforce mechanical rules.

This system is written for agents. It does not attempt to double as a traditional human contribution guide.

## 2. Confirmed Project Rules

### 2.1 Autonomy

Agents may autonomously:

- inspect source, configuration, history, Issues, and PRs;
- create and maintain Issues;
- create task branches;
- modify code, tests, configuration, and documentation within Issue scope;
- start local development dependencies;
- run static checks, builds, and tests;
- create atomic commits;
- push task branches;
- create and update Draft PRs;
- address verified review feedback and CI failures;
- create follow-up Issues for out-of-scope findings.

Human approval is required for:

- merging a PR;
- production environment operations;
- using or changing real credentials;
- irreversible data migration or deletion;
- destructive Git operations;
- expanding work into another primary Issue;
- intentionally changing public compatibility or settled product semantics.

### 2.2 Issue-driven work

Every repository change must have a valid Issue before an implementation branch is created. Emergency security containment may proceed first only when delay materially increases harm; the agent must immediately create the Issue and document the exception.

One PR resolves one primary Issue. Out-of-scope findings become separate Issues rather than opportunistic changes in the current PR.

### 2.3 Version-line branch model

`main` represents the latest released version and is not the default development base.

```text
main
└── <major>.0-dev
    └── <major>.<minor>-dev
        └── task branch
```

Major versions use long-lived development branches such as `3.0-dev` and `4.0-dev`. A minor-version development line branches from its owning major-version development line. A task branch starts from the Issue's target version line and merges back into that line.

Cross-version ports require independent Issues and PRs. Stacked PRs are allowed only when they declare dependencies, merge order, and the final target version line.

### 2.4 Test-first development

Features, bug fixes, refactors, and behavior changes use strict RED-GREEN-REFACTOR:

1. Write the smallest test that expresses the required behavior.
2. Run it and confirm that it fails for the expected reason.
3. Write the minimum production change required to pass.
4. Run the target test and relevant regressions.
5. Refactor only while the suite remains green.

Production code written before the failing test must be discarded and implemented again from the test. Documentation-only, comment-only, and provably behavior-neutral mechanical changes may be exempt, but the PR must state why no behavioral test applies.

### 2.5 Language

- Agent Skills, engineering references, automation, code identifiers, new code comments, and new log messages use English.
- Issue and PR titles use English.
- Issue and PR body prose uses Chinese.
- Commit messages use English Conventional Commits.
- Existing Chinese comments are not rewritten without a task-local reason.
- Public error text preserves existing compatibility unless the Issue explicitly changes the contract.

### 2.6 Formatting

Go changes must pass `gofmt`. C++ changes follow the neighboring file's established style. Agents must not reformat entire C++ files or mix unrelated formatting changes with behavioral work. Repository-wide C++ formatting requires its own Issue and PR.

### 2.7 Architecture and core-flow synchronization

Any change to system architecture, service boundaries, infrastructure topology, or a core flow must update the affected ChatNow Skill and references in the same PR. A follow-up Issue is not an acceptable substitute.

This includes changes to:

- service ownership and communication boundaries;
- HTTP, brpc, MQ, WebSocket, or storage flows;
- authentication and authorization propagation;
- persistence, indexing, caching, sequencing, idempotency, retry, Outbox, push, and ACK semantics;
- infrastructure dependencies, deployment topology, and service configuration;
- test architecture and mandatory engineering workflows.

## 3. Verified Architecture Context

The architecture reference Skill must derive facts from source, Protobuf, executable configuration, CMake, Docker Compose, and tests. README files and historical design documents are secondary evidence.

### 3.1 Technology stack

| Concern | Technology |
|---|---|
| Production language | C++17 |
| Build | CMake |
| External API | HTTP with Protobuf payloads |
| Internal RPC | brpc and Protobuf |
| Long-lived client delivery | WebSocket |
| Database | MySQL 8 with ODB |
| Cache and coordination | Redis 7 Cluster plus local L1 caches |
| Message broker | RabbitMQ through AMQP-CPP/libev |
| Search | Elasticsearch 7 |
| Object storage | MinIO through the S3-compatible AWS C++ SDK |
| Service discovery and leases | etcd |
| Authentication | JWT HS256 with multi-key rotation support |
| Logging | spdlog JSON lines with propagated trace context |
| Integration and system tests | Go |
| Automation | GitHub Actions |

### 3.2 Services

| Service | Primary responsibility |
|---|---|
| Gateway | HTTP entry, JWT validation, request routing |
| Identity | Registration, login, token issuance, profiles, user lookup |
| Relationship | Friend requests, relationships, blocking |
| Conversation | Conversation lifecycle, membership, unread and pin state |
| Transmite | Message ingest, authorization, sequencing, idempotency, MQ publication |
| Message | Persistence, timelines, search indexing, history and synchronization |
| Media | S3-compatible upload, multipart upload, deduplication, quota, speech operations |
| Presence | Presence state and typing-related coordination |
| Push | WebSocket connections, routing, cross-instance delivery, resend and ACK handling |

### 3.3 Core message flow

The core send path is:

```text
Client
→ Gateway HTTP
→ Transmite brpc
→ RabbitMQ message exchange
→ Message database consumer
→ MySQL persistence and timeline updates
→ push exchange/queue
→ Push service
→ local or cross-instance WebSocket delivery
→ client ACK
→ Message read-ack convergence
```

The architecture reference must describe the following invariants without overstating exactly-once delivery:

- request and message identifiers support idempotent retries;
- conversation and user sequences support ordered synchronization;
- MQ and remote delivery are at-least-once and require idempotent effects;
- persistence precedes normal push publication;
- Outbox/reaper paths cover declared asynchronous delivery failures;
- Redis and local caches are acceleration or coordination layers whose failure behavior must be explicit;
- Message remains the source of truth for stored messages;
- Push owns live routes, resend state, cross-instance fanout, and client ACK ingestion;
- trace context propagates through HTTP, RPC, MQ, logs, and notifications where supported.

### 3.4 Current test architecture

The repository uses a pure Go test stack. New or restored C++ test suites are prohibited.

| Layer | Location and tag | Purpose |
|---|---|---|
| L0 Build | CI | C++ build, Go vet, and formatting |
| L1 BVT | `tests/bvt`, `bvt` | Fast core-path gate |
| L2 Functional | `tests/func`, `func` | Service APIs, error paths, boundaries |
| L3 Scenario | `tests/func`, `func` | Cross-service workflows and consistency |
| L4 Performance | `tests/perf`, `perf` | Throughput and latency baselines |
| Reliability | `tests/reliability`, `reliability` | Failure injection and recovery |

The test framework provides shared HTTP and WebSocket clients, fixtures, cleanup, and direct MySQL, Elasticsearch, and MinIO verification. Tests use condition polling instead of fixed readiness sleeps and use stable case IDs defined by the test Skill.

## 4. Chosen Organization

The repository uses one root `AGENTS.md` and no directory-local `AGENTS.md` files. Detailed rules are Skills under `.agents/skills/`.

```text
AGENTS.md
.agents/skills/
├── chatnow-orienting/
├── chatnow-developing/
├── chatnow-testing/
├── chatnow-creating-issues/
├── chatnow-submitting-pull-requests/
├── chatnow-using-git/
├── chatnow-verifying-changes/
├── chatnow-securing-changes/
└── chatnow-maintaining-documentation/
```

Each Skill contains:

- a concise `SKILL.md` with only `name` and `description` frontmatter;
- `agents/openai.yaml` with generated interface metadata;
- references only when detailed project knowledge is necessary;
- scripts only when deterministic, reusable behavior belongs to that Skill.

Skills do not contain README, installation, quick-reference, changelog, or process-history files.

## 5. Root AGENTS.md Contract

`AGENTS.md` contains only:

1. project invariants;
2. a Skill trigger table;
3. the standard execution sequence;
4. human approval boundaries;
5. rule precedence.

Rule precedence is:

```text
User instruction
→ AGENTS.md project invariants
→ Triggered ChatNow Skills
→ Existing repository conventions
→ Agent judgment
```

Standard execution is:

```text
Request
→ Orient
→ Create or validate Issue
→ Select version line
→ Create task branch
→ RED
→ GREEN
→ REFACTOR
→ Verify
→ Self-review
→ Commit and push
→ Create or update Draft PR
→ Address review feedback
→ Human merge approval
```

Mandatory routing:

| Situation | Required Skill |
|---|---|
| First task, architecture question, or cross-service change | `chatnow-orienting` |
| Repository change without a valid Issue | `chatnow-creating-issues` |
| Base selection, branch, commit, sync, conflict, or backport | `chatnow-using-git` |
| Feature, bug fix, behavior change, or refactor | `chatnow-testing` before implementation |
| Production code, Protobuf, config, or infrastructure change | `chatnow-developing` |
| Auth, input, data, file, network, credential, or log change | `chatnow-securing-changes` |
| Documentation or engineering-instruction change | `chatnow-maintaining-documentation` |
| Completion, commit, push, or PR preparation | `chatnow-verifying-changes` |
| PR creation, update, or review readiness | `chatnow-submitting-pull-requests` |

`AGENTS.md` uses explicit `REQUIRED SKILL` language and does not duplicate Skill details.

## 6. Skill Contracts

Every Skill follows a common internal shape:

```text
Trigger metadata
→ Core principle
→ Required inputs
→ Mandatory workflow
→ Output contract
→ Stop conditions
→ Common mistakes or rationalizations
→ Conditional references
```

### 6.1 chatnow-orienting

Use for first-time repository work, architectural analysis, cross-service changes, and core-flow changes.

It must:

- resolve the target version line and current commit;
- cross-check source, Proto, config, Compose, build files, tests, and history;
- identify affected services, entry points, stores, synchronous and asynchronous boundaries, and invariants;
- distinguish executable facts from proposals;
- require Skill/reference updates when architecture or core flows change.

Resources:

```text
references/technology-stack.md
references/repository-map.md
references/core-flows.md
```

`core-flows.md` covers HTTP-to-brpc routing, message ingest and persistence, search indexing, push and ACK, authentication, conversation and relationship operations, presence, media upload, Redis coordination, and Outbox recovery.

### 6.2 chatnow-developing

Use for production C++, Proto, configuration, persistence, caching, MQ, and infrastructure changes.

It must require agents to:

- identify the nearest established pattern and relevant invariants;
- keep the diff minimal and avoid unrelated refactors;
- define synchronous failure, asynchronous failure, retry, timeout, and idempotency behavior;
- preserve RPC authentication, error mapping, and closure lifetime;
- define MQ topology and confirm/retry semantics;
- define Redis fail-open, fail-closed, or degraded behavior;
- define transaction boundaries and repeated-execution results;
- use English for new identifiers, comments, and logs;
- link every behavior change to a test-first cycle.

Its output contract is a concise statement of changed invariants, minimal scope, failure behavior, compatibility impact, and tests.

### 6.3 chatnow-testing

Use before implementing any feature, bug fix, refactor, or behavior change and whenever tests are added or changed.

Its iron law is:

```text
NO PRODUCTION CODE WITHOUT A TEST THAT FAILED FOR THE EXPECTED REASON FIRST.
```

It must define:

- RED-GREEN-REFACTOR and restart behavior after a violation;
- the L0-L4 plus Reliability model;
- `bvt`, `func`, `perf`, and `reliability` tags;
- the minimum sufficient test layer for each change;
- case-ID allocation and function naming;
- shared clients, fixtures, cleanup, and direct-store verification;
- condition polling and resource cleanup requirements;
- local and CI commands;
- reporting rules when the Linux full stack is unavailable.

It must prohibit C++ tests, tests-after, false RED evidence, fixed readiness sleeps, duplicated setup, leaked test state, and claims that static compilation proves runtime behavior.

Resources:

```text
references/framework.md
references/case-catalog.md
```

### 6.4 chatnow-creating-issues

Use before any repository change and when an out-of-scope problem is discovered.

It must require:

- an English title and Chinese body prose;
- exactly one primary problem or deliverable;
- a target version line;
- source, log, test, or behavioral evidence;
- scope and non-goals;
- measurable acceptance criteria;
- a test-first plan;
- risk and security impact;
- architecture/core-flow impact and expected Skill updates.

Agents may update the Issue as evidence improves but may not rewrite acceptance criteria after implementation merely to fit the produced code.

### 6.5 chatnow-submitting-pull-requests

Use before pushing for review or creating, updating, or marking a PR ready.

It must require:

- an English Conventional Commit-style title and Chinese body prose;
- Draft-first creation;
- one primary Issue;
- the correct version-line base;
- scope and non-goals;
- architecture/core-flow impact and Skill updates;
- RED, GREEN, and regression evidence;
- explicit unverified items;
- security, compatibility, migration, and rollback impact;
- dependency and merge order for stacked PRs;
- a self-review of the complete diff.

Agents may push and update Draft PRs but may not merge them.

### 6.6 chatnow-using-git

Use for base selection, branch creation, commit, synchronization, conflict handling, stacking, or cross-version ports.

Task branch patterns are:

```text
feat/<issue-number>-<slug>
fix/<issue-number>-<slug>
refactor/<issue-number>-<slug>
test/<issue-number>-<slug>
docs/<issue-number>-<slug>
chore/<issue-number>-<slug>
```

It must require:

- the Issue's target version line as base;
- an Issue number in the task branch;
- English Conventional Commits;
- atomic, reviewable commits;
- no unrelated formatting;
- merge-base verification before PR creation;
- separate Issues and PRs for cross-version work;
- explicit stacked-PR dependencies.

It must prohibit rewriting shared history and destructive repair without human approval.

### 6.7 chatnow-verifying-changes

Use before claiming completion, committing, pushing, or opening a PR.

It defines this evidence ladder:

1. static checks;
2. compilation;
3. target test;
4. same-layer regression;
5. BVT, functional, and scenario testing;
6. performance or reliability testing;
7. CI.

It must require fresh commands and outputs and must label each check `passed`, `failed`, `not run`, or `blocked`. A Draft PR may document unavailable dynamic verification, but an agent may not claim that unexecuted tests passed or mark the work ready on static evidence alone.

### 6.8 chatnow-securing-changes

Use for authentication, authorization, input, user data, media, file paths, database/search queries, networking, credentials, logging, or production-impacting work.

It must require:

- explicit trust boundaries and server-derived identity;
- parameterized SQL and safe Elasticsearch query construction;
- constrained file paths and object keys;
- credential and personal-data redaction;
- English, structured, non-sensitive logs;
- secure failure for high-risk uncertainty;
- regression and adversarial-path tests;
- human approval for production, real credentials, and irreversible data operations.

### 6.9 chatnow-maintaining-documentation

Use for Skills, architecture references, API/operations documentation, Issue text, and PR text.

It must enforce the language rules, fact-source precedence, version/date labeling, link integrity, and the same-PR architecture/core-flow synchronization rule. It must prohibit duplicate README, quick-reference, changelog, migration-history, or stale-status files that do not directly support an agent workflow.

## 7. GitHub Interaction Design

### 7.1 Issue Forms

```text
.github/ISSUE_TEMPLATE/config.yml
.github/ISSUE_TEMPLATE/bug.yml
.github/ISSUE_TEMPLATE/feature.yml
.github/ISSUE_TEMPLATE/refactor.yml
.github/ISSUE_TEMPLATE/engineering.yml
```

Blank Issues are disabled. Every form collects target version, evidence, scope, non-goals, acceptance criteria, test-first plan, risk/security impact, architecture/core-flow impact, and expected Skill updates. Form labels and machine markers use English; authored body prose uses Chinese.

### 7.2 PR template

`.github/pull_request_template.md` requires:

- primary Issue;
- target version line;
- scope and non-goals;
- architecture/core-flow impact;
- updated Skills and references;
- RED, GREEN, and regression evidence;
- security and compatibility impact;
- unverified items;
- rollback plan;
- stacked-PR dependencies.

### 7.3 Policy workflow

`.github/workflows/agent-policy.yml` checks:

- English Conventional Commit-style PR title;
- Issue number in the task branch;
- one linked primary Issue;
- non-empty required PR sections;
- base/version-line consistency;
- no ordinary task PR to `main`;
- English Conventional Commits;
- RED, GREEN, and regression evidence;
- explicit unverified items;
- valid Skill structure and metadata;
- valid internal references;
- architecture/core-flow impact declaration and required Skill synchronization.

## 8. Agent Policy CLI

To keep repository tests in Go, policy validation is implemented as a Go CLI in the existing tests module:

```text
tests/cmd/agent-policy/main.go
tests/pkg/agentpolicy/
├── branch.go
├── commits.go
├── issue.go
├── pull_request.go
├── skill_sync.go
├── skills.go
├── branch_test.go
├── commits_test.go
├── issue_test.go
├── pull_request_test.go
├── skill_sync_test.go
└── skills_test.go
```

Commands are:

```bash
go run ./cmd/agent-policy issue
go run ./cmd/agent-policy pull-request
go run ./cmd/agent-policy commits
go run ./cmd/agent-policy branch
go run ./cmd/agent-policy skill-sync
go run ./cmd/agent-policy skills
```

`tests/Makefile` adds `test-agent-policy`. The policy CLI itself is implemented test-first.

### 8.1 Architecture-sensitive paths

The synchronization check considers at least:

```text
proto/**
*/source/**
common/infra/**
common/mq/**
common/auth/**
common/dao/**
conf/**
docker-compose.yml
docker/**
CMakeLists.txt
*/CMakeLists.txt
```

The PR declares `architecture-impact` and `core-flow-impact` as `yes` or `no`, explains the judgment, and lists affected Skills/references. If either value is `yes`, the same PR must update the relevant `chatnow-orienting` reference and any affected testing, development, security, Git, or documentation Skill.

Path detection is a conservative prompt for semantic review, not proof that no impact exists. The PR Skill requires the agent to perform the semantic check. Incorrectly declaring no impact is a process violation that must be corrected before merge.

## 9. Failure and Blocking Behavior

| Condition | Required behavior |
|---|---|
| GitHub unavailable and no Issue exists | Continue read-only research only; do not branch or implement |
| Target version ambiguous | Infer from Issue/milestone/history; ask only if multiple targets remain valid |
| Wrong branch base | Stop new commits and migrate non-destructively |
| Skills conflict | Apply `AGENTS.md` precedence and create an engineering Issue to remove the contradiction |
| Skill/reference contradicts executable source | Treat executable source as fact and fix the Skill in the same PR |
| Architecture/core flow changes | Update affected Skill/reference in the same PR |
| Policy workflow false positive | Fix policy or request an explicit human exception; never bypass silently |
| Linux full stack unavailable | Create Draft PR with dynamic verification gap; do not claim ready |
| Test cannot be made RED | Determine whether behavior already exists; never fabricate RED evidence |
| Out-of-scope defect discovered | Create a separate Issue |
| Security or data risk uncertain | Fail safely and request human direction |

## 10. Skill Development and Evaluation

Skills are built one at a time using documentation TDD.

For each Skill:

1. Initialize the Skill with the standard Skill creator.
2. Define realistic pressure scenarios without the Skill.
3. Run a baseline and capture exact omissions and rationalizations.
4. Write the minimum Skill that addresses observed failures.
5. Re-run the same scenarios with the Skill.
6. Add variations and counterexamples to expose loopholes.
7. Refine and re-test until behavior is stable.
8. Run structural validation.
9. Validate `agents/openai.yaml` against `SKILL.md`.
10. Validate references and scripts.
11. Commit the verified Skill before starting the next Skill.

Pressure scenarios include:

- a request to make a quick fix without an Issue;
- code already written before tests;
- a task branch or PR incorrectly targeting `main`;
- static validation presented as runtime success;
- an architecture change without a Skill update;
- unrelated fixes added for convenience;
- credentials exposed in logs or PR text;
- a stacked PR without dependency declarations.

No Skill is deployed solely on the basis of prose review.

## 11. Delivery Order

1. Establish Skill initialization and evaluation support.
2. Implement and verify `chatnow-orienting`.
3. Implement and verify `chatnow-creating-issues`.
4. Implement and verify `chatnow-using-git`.
5. Implement and verify `chatnow-testing`.
6. Implement and verify `chatnow-developing`.
7. Implement and verify `chatnow-securing-changes`.
8. Implement and verify `chatnow-verifying-changes`.
9. Implement and verify `chatnow-submitting-pull-requests`.
10. Implement and verify `chatnow-maintaining-documentation`.
11. Implement Issue Forms and PR template.
12. Implement the Go policy CLI test-first.
13. Implement the policy workflow.
14. Create root `AGENTS.md` only after all routed Skills exist and pass validation.
15. Run full workflow pressure scenarios and final self-review.

Each Skill has its own atomic commit. Templates, policy tooling, workflow, and `AGENTS.md` are independently reviewable commits.

## 12. Complete Deliverable Tree

```text
AGENTS.md

.agents/skills/
├── chatnow-orienting/
│   ├── SKILL.md
│   ├── agents/openai.yaml
│   └── references/
│       ├── technology-stack.md
│       ├── repository-map.md
│       └── core-flows.md
├── chatnow-developing/
│   ├── SKILL.md
│   └── agents/openai.yaml
├── chatnow-testing/
│   ├── SKILL.md
│   ├── agents/openai.yaml
│   └── references/
│       ├── framework.md
│       └── case-catalog.md
├── chatnow-creating-issues/
│   ├── SKILL.md
│   └── agents/openai.yaml
├── chatnow-submitting-pull-requests/
│   ├── SKILL.md
│   └── agents/openai.yaml
├── chatnow-using-git/
│   ├── SKILL.md
│   └── agents/openai.yaml
├── chatnow-verifying-changes/
│   ├── SKILL.md
│   └── agents/openai.yaml
├── chatnow-securing-changes/
│   ├── SKILL.md
│   └── agents/openai.yaml
└── chatnow-maintaining-documentation/
    ├── SKILL.md
    └── agents/openai.yaml

.github/
├── ISSUE_TEMPLATE/
│   ├── config.yml
│   ├── bug.yml
│   ├── feature.yml
│   ├── refactor.yml
│   └── engineering.yml
├── pull_request_template.md
└── workflows/
    └── agent-policy.yml

tests/
├── cmd/agent-policy/main.go
├── pkg/agentpolicy/*.go
└── Makefile
```

## 13. Acceptance Criteria

- All nine Skills pass structural validation and independent baseline/with-Skill pressure scenarios.
- Every Skill description contains precise trigger conditions and does not summarize the workflow as a shortcut.
- Every `agents/openai.yaml` matches its Skill.
- `AGENTS.md` remains short and contains only invariants, routing, precedence, and approvals.
- Architecture references match current source, configuration, Compose, Proto, and the pure Go test architecture.
- Skills and references use English.
- Issue and PR titles use English; body prose uses Chinese.
- Issue-driven work, version-line targeting, TDD, Draft-first PRs, and human merge approval are mechanically checked where possible.
- Architecture or core-flow changes without corresponding Skill/reference updates fail policy validation.
- The policy CLI is implemented test-first and its tests pass.
- An agent can progress from a valid Issue through a pushed Draft PR without routine human confirmation.
- Merge, production, real-credential, irreversible-data, destructive-Git, and scope-expansion boundaries remain human-controlled.
- The repository contains no duplicate Skill README, migration narrative, or obsolete test-framework description.

## 14. Non-goals

- Creating human-oriented contributor documentation.
- Introducing repository-wide C++ formatting.
- Allowing agents to merge PRs or operate production.
- Replacing semantic review with path-based policy checks.
- Maintaining a second test framework outside the pure Go testing architecture.
- Duplicating the same rule across `AGENTS.md`, multiple Skills, and references.
