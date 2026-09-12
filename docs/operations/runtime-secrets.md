# Runtime Secret Management

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

This is the sole canonical inventory and operational contract for ChatNow runtime secrets. It distinguishes implemented consumers from explicitly out-of-scope credential classes.

## Current behavior

The following behavior is implemented at the verified commit:

| Credential | Current consumers | Current source |
|---|---|---|
| JWT HS256 JSON document | Identity signs and verifies; Gateway and Push verify | `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` through the common resolver |
| MySQL password | Conversation, Identity, Media, Message, and Relationship | Service-specific direct or `_FILE` input through the common resolver |
| RabbitMQ password | Transmite, Message, and Push | Service-specific direct or `_FILE` input through the common resolver |
| SMTP password | Identity | `CHATNOW_IDENTITY_SMTP_PASSWORD` or its `_FILE` companion |
| S3 application access and secret keys | Media | Separate service-specific direct or `_FILE` inputs; non-secret S3 settings remain in `conf/media.json` |
| MinIO bootstrap credential | Root Compose `minio` and `minio-init` services | Required Compose deployment environment references |
| MySQL root and RabbitMQ bootstrap passwords | Root Compose infrastructure and one-shot initializers | Required Compose deployment environment references |

Redis has no configured password or ACL consumer. The ASR helper can accept credentials, but Media startup does not currently wire them. The CI workflow does not pull real credentials; local and CI stack inputs must be synthetic.

Tracked runtime literals have been removed from the scoped source, configuration, Compose, and test-runtime surfaces. Do not reintroduce, quote, log, hash, fingerprint, or copy credential values into Issues, PRs, tests, artifacts, or replacement documentation.

### Implemented resolver inputs

| Logical secret | Direct input | File input |
|---|---|---|
| JWT document | `CHATNOW_JWT_CONFIG` | `CHATNOW_JWT_CONFIG_FILE` |
| Identity MySQL password | `CHATNOW_IDENTITY_MYSQL_PASSWORD` | `CHATNOW_IDENTITY_MYSQL_PASSWORD_FILE` |
| Conversation MySQL password | `CHATNOW_CONVERSATION_MYSQL_PASSWORD` | `CHATNOW_CONVERSATION_MYSQL_PASSWORD_FILE` |
| Relationship MySQL password | `CHATNOW_RELATIONSHIP_MYSQL_PASSWORD` | `CHATNOW_RELATIONSHIP_MYSQL_PASSWORD_FILE` |
| Message MySQL password | `CHATNOW_MESSAGE_MYSQL_PASSWORD` | `CHATNOW_MESSAGE_MYSQL_PASSWORD_FILE` |
| Media MySQL password | `CHATNOW_MEDIA_MYSQL_PASSWORD` | `CHATNOW_MEDIA_MYSQL_PASSWORD_FILE` |
| Transmite RabbitMQ password | `CHATNOW_TRANSMITE_MQ_PASSWORD` | `CHATNOW_TRANSMITE_MQ_PASSWORD_FILE` |
| Message RabbitMQ password | `CHATNOW_MESSAGE_MQ_PASSWORD` | `CHATNOW_MESSAGE_MQ_PASSWORD_FILE` |
| Push RabbitMQ password | `CHATNOW_PUSH_MQ_PASSWORD` | `CHATNOW_PUSH_MQ_PASSWORD_FILE` |
| Identity SMTP password | `CHATNOW_IDENTITY_SMTP_PASSWORD` | `CHATNOW_IDENTITY_SMTP_PASSWORD_FILE` |
| Media S3 access key | `CHATNOW_MEDIA_S3_ACCESS_KEY` | `CHATNOW_MEDIA_S3_ACCESS_KEY_FILE` |
| Media S3 secret key | `CHATNOW_MEDIA_S3_SECRET_KEY` | `CHATNOW_MEDIA_S3_SECRET_KEY_FILE` |

Compose bootstrap variables (`CHATNOW_MYSQL_ROOT_PASSWORD`, `CHATNOW_RABBITMQ_BOOTSTRAP_PASSWORD`, `CHATNOW_MINIO_ROOT_USER`, and `CHATNOW_MINIO_ROOT_PASSWORD`) are required deployment inputs, not common-resolver inputs. `CHATNOW_RABBITMQ_BOOTSTRAP_USER` may override the synthetic local bootstrap user name. Do not append `_FILE` and assume Compose supports it.

The root Compose one-shot initializers receive only the credentials they need to converge disposable local application identities:

- `mysql-init` uses the MySQL root credential plus the five service-specific MySQL passwords to create or update table-scoped users and grants.
- `rabbitmq-init` uses the RabbitMQ bootstrap credential plus the Transmite, Message, and Push passwords to create or update scoped users and permissions.
- `minio-init` uses the MinIO root credential plus the Media S3 application credentials to create or update the two buckets, policies, and application identity.

RabbitMQ secrets are sent in Management API JSON bodies rather than command arguments. MinIO feeds root and application secret material to `mc` through standard input, isolates `mc` state in a temporary configuration directory, and removes it on exit. These initialization boundaries do not make bootstrap credentials application inputs. Application containers continue to receive only their own direct or `_FILE` resolver inputs. See [Compose Runtime Operations](compose-runtime.md) for ordering and readiness; neither document is evidence that a cold start has passed.

## Current injection contract

For every migrated logical secret named `NAME`, the common resolver accepts exactly one source:

- `NAME`: the secret value supplied directly by the process environment; or
- `NAME_FILE`: a locator for a runtime-mounted file. The resolver removes at most one trailing line ending and otherwise preserves the file content.

The resolver:

1. fail closed when both variables are set, when a required secret is missing, or when the resolved value is empty;
2. reject a symlink, non-regular file, unreadable file, file owned by neither the effective runtime user nor root, or any group/other access or special permission bits;
3. read once during startup into process memory, close the file, and avoid copying the value into gflags, process arguments, logs, errors, metrics, traces, crash annotations, or generated artifacts;
4. report only the logical secret name, selected source type, and a stable non-sensitive error category;
5. preserve existing service ownership and fail startup before accepting traffic when a required secret is invalid.

The deployment must give the runtime identity read-only access to the mounted file. Prefer ownership by that identity (or an explicitly approved root-owned read-only mount) and mode `0400`; do not allow group/other permission bits. Mount each service only the secrets it consumes.

Do not add a silent compatibility fallback to tracked configuration, compiled defaults, or command-line values. A staged migration may support legacy input only when the scoped Issue names the transition, makes precedence unambiguous, and proves eventual removal.

## Credential classes and boundaries

- Application credentials are the identities used by ChatNow services for MySQL, RabbitMQ, SMTP, and S3. Injection protects delivery but does not prove least privilege; deployment roles and configured usernames must be reviewed separately and narrowed before production.
- Bootstrap credentials create or rotate application identities and belong to the deployment system, not an application container.
- JWT signing keys belong only to the authentication deployment boundary. Gateway and Push require verifier access to the shared HS256 key set under the current algorithm, but they must not become rotation owners.
- Redis credentials remain out of scope until Redis ACL/password consumption is implemented and assigned to an Issue.

Never reuse a bootstrap credential as an application credential. Never mount a full environment's secret bundle into every service.

## Local development and CI

- Use unique synthetic values generated for the disposable environment. They must not be copied from staging or production and must carry no external privilege.
- Inject them at runtime through ignored local environment files or ephemeral secret mounts. `.env` being ignored does not make it an approved production store.
- CI must source synthetic values from ephemeral job setup or the CI secret mechanism, mask values, avoid command tracing, and tear down volumes and temporary files on every exit path.
- CI generates disposable values with `scripts/create_test_env.py --github-env`, registers masks, and injects them into each job. The generator refuses an existing `.env` or `middle/data`; gate results must still be verified for the current commit.
- Scanner exemptions must match exact synthetic fixtures or documented API examples. Do not exempt an entire `conf/`, `tests/`, `docs/`, Compose, or source subtree.
- Tests may assert source selection and error categories, but must not print the resolved value or any derivative.

## Deployment procedure

This procedure applies only to the implemented consumers listed above. A new credential class requires a scoped Issue, allowlist entry, consumer wiring, and pure-Go policy coverage before this procedure applies.

1. Inventory the service and logical secret, its least-privileged owner, current source, approved provider, rollout owner, and rollback version.
2. Obtain human approval before any real credential or production operation.
3. Create or rotate the value in the approved provider without exposing it to a terminal, ticket, log, or artifact.
4. Publish either `NAME` or `NAME_FILE`, never both. For a file, set the required owner and restrictive mode before starting the process.
5. Roll one bounded unit, verify startup and dependency authentication from non-sensitive outcomes, then continue according to the service's availability policy.
6. After fleet convergence, remove the legacy source and prove that its absence cannot trigger a tracked/default fallback.
7. Revoke the superseded credential only after the rollback and overlap conditions for that credential class are satisfied.

JWT rotation has verifier/signer ordering and overlap requirements; use [JWT Key Rotation](jwt-key-rotation.md).

## Rollback

Before rollout, preserve the last approved provider version and deployment manifest, never a plaintext copy in the repository.

- If startup rejects the new source, keep the instance out of service and restore the prior provider version or mount metadata.
- If dependency authentication fails, stop the rollout, restore the previous secret reference, and restart only the affected bounded unit.
- If the old credential is still valid, restore it through the provider. If it has already been revoked, issue a new approved credential; never recover it from logs, shell history, or Git.
- A rollback must not re-enable a tracked literal, compiled default, command-line secret, or direct/file ambiguity.

Record environment, service, logical secret name, source type, provider version, rollout timestamps, non-sensitive outcome, approval reference, and rollback decision. Never record a value or credential-derived fingerprint.

## Incident and rotation boundary

Real rotation, revocation, provider access, and production rollout are human-approval boundaries. Suspected disclosure is an incident: stop ordinary migration, preserve non-secret evidence, notify the designated owner, and follow the approved revocation and user-impact plan. Availability pressure does not authorize restoring a suspected-compromised credential.
