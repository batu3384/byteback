# NSIS clean-box smoke log (operator — do not fake PASS)

Use with runbook §3. Lab unsigned installer is **not** PASS until this VM run is recorded.

## Installer under test

- Path (lab machine): `release/Byteback Setup 0.1.0.exe` (gitignored; rebuild via `npm run release:dist:unsigned`)
- SHA-256 (2026-10-28 lab build): `06DB68C065FEDD1618A0AA5F5CAAD33F52E044A1085B14FD3717FFFD90301A62`
- Signed build: replace hash after `npm run release:dist` + `signtool verify`

## VM run

- Date:
- VM OS (clean / snapshot id):
- Install result:
- Launch: app opens without crash
- Smoke: scan-from-image or engine load confirmed (`byteback_engine.node`)
- PASS / FAIL:

## Sign-off

- Operator:
- Notes:
