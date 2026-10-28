# Authenticode release sign log (operator — do not fake PASS)

Runbook §2. Checklist Authenticode stays `[ ]` until `signtool verify` succeeds on the shipped installer.

## Build

- Date:
- Command: `npm run release:dist` (or `release-signed.yml` run id)
- Installer path:
- SHA-256:

## Verification

- `signtool verify /pa "<installer>"` exit code:
- Signer / timestamp (paste relevant signtool output):
- PASS / FAIL:

## Sign-off

- Operator:
- Notes:
