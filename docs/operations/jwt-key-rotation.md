# JWT Key Rotation

Target version: `3.0-dev`
Status: Current
Verified: 2026-07-22

This runbook rotates the HS256 signing key without invalidating tokens that are still within their accepted lifetime. It never authorizes a production operation: using or changing real keys and operating on production require explicit human approval naming the environment, identities, time window, and rollback owner.

## Current behavior

- Identity signs and verifies JWTs. Gateway and Push verify JWTs.
- Each process resolves the complete JSON document from exactly one of `CHATNOW_JWT_CONFIG` or `CHATNOW_JWT_CONFIG_FILE` at startup. There is no hot reload.
- A token carries a key ID. Verification succeeds only while that ID remains in the local key map and the token otherwise passes validation.
- The tracked `conf/auth.json` is repository configuration, not an operational secret store. Never edit or commit it to rotate a deployed key.
- The shared resolver enforces the source and mounted-file controls in [Runtime Secret Management](runtime-secrets.md). Never pass the document through a removed `auth_config` flag or restore a tracked-file fallback.

## Preconditions

Before changing anything:

1. Obtain the required human approval and name the rollout and rollback owners.
2. Confirm the deployed commit, every Identity, Gateway, and Push instance, the configured token lifetimes, maximum clock skew, and maximum rollout delay.
3. Confirm an approved secret provider can publish the same JWT document to every consumer through the direct input or, preferably, an untracked read-only file referenced by `CHATNOW_JWT_CONFIG_FILE` with the required ownership and permissions.
4. Record only key IDs and deployment state. Never print, copy into a ticket, log, hash, fingerprint, prefix, suffix, or otherwise derive diagnostic material from a key value.
5. Verify that old and new configurations can be restored from the provider without using repository history.

The overlap window must exceed the longest token lifetime that must remain valid, plus clock skew and rollout delay. Use the deployed values; do not rely on repository examples.

## Rotation procedure

### 1. Prepare the new key

Generate a high-entropy key inside the approved secret system. Assign a new, non-reused key ID. Do not generate it with a command that writes the value to terminal output, shell history, CI output, or an artifact.

Publish an overlap key map containing both the old and new key IDs while leaving `current_kid` on the old key. Validate schema and minimum key length inside the protected secret workflow.

### 2. Deploy the overlap verifier set

Roll Gateway and Push instances first so every verification path accepts both key IDs. Then roll Identity while it still signs with the old key. A process must not receive traffic until startup has loaded the overlap set successfully.

Verify, without exposing token or key material, that:

- pre-rollout access tokens remain accepted by Gateway and Push, and pre-rollout refresh tokens remain accepted by Identity;
- Identity still issues tokens with the old key ID;
- authentication error and startup-failure rates remain within the approved bounds.

Stop and roll back if any verifier does not accept the old key or if the fleet is not converged.

### 3. Switch the signer

In the secret provider, change only `current_kid` to the new key ID while retaining both keys. Roll Identity so new tokens use the new key. Roll Gateway and Push as well if the deployment mechanism does not guarantee they already have the identical overlap file.

Verify that newly issued access tokens are accepted by Gateway and Push and newly issued refresh tokens are accepted by Identity. Also verify an unexpired old-key access token through Gateway and Push and an unexpired old-key refresh token through Identity. Observe only key IDs, bounded outcome counters, and trace/request IDs.

### 4. Hold the overlap

Keep both keys until the last old-key token that the product promises to accept has expired. The hold starts after the last Identity instance stopped signing with the old key and lasts longer than the applicable maximum token lifetime plus clock skew and rollout delay.

### 5. Retire the old key

After approval to finish the rotation, remove the old key from the provider-managed key map and roll Gateway, Push, and Identity. Confirm all instances loaded the new-only set and that new-key tokens still pass both HTTP and WebSocket authentication.

Retire provider versions according to the approved retention policy. Do not place an archived key in the repository or routine logs.

## Rollback

- Before old-key removal: set `current_kid` back to the old key in the provider, roll Identity, and verify old-key signing and all verification paths. Both keys remain present.
- After old-key removal: restore the previous overlap set from the provider, roll Gateway and Push first, then Identity, and verify old tokens before switching the signer.
- If the old key is suspected compromised, do not restore it merely to preserve availability. Escalate to the incident owner for an explicitly approved revocation plan and user/session impact decision.

Rollback does not permit logging or exposing either key. Record key IDs, rollout versions, timestamps, instance health, and approval references only.

## Failure handling

- `current_kid` absent from the key map or a key rejected at startup: keep the instance out of service and restore the last approved provider version.
- Widespread invalid-token responses after a rollout: stop, compare non-secret deployment versions and key IDs across Identity, Gateway, and Push, then follow the applicable rollback stage.
- Mixed fleet state: stop the signer switch or retirement. Converge verifiers before allowing Identity to sign with a key they may not accept.
- Provider or mount unavailable: fail the rollout. Never fall back to a tracked file, compiled value, or command-line secret.
