# Phase 4 operator runbook (honest PASS only)

Engineering Phases 1–3 are **Done** (`npm run verify:audit:full`). This runbook closes the remaining **Open** rows in `OBJECTIVE_CHECKLIST.md`.

## 1. Field USB (rows A/B/C)

1. `npm run field:usb:preflight`
2. Run each checklist row on real media (not lab fixtures).
3. Record evidence in `audit/FIELD_USB_LOG.md` (MD5 for row B).
4. Check `[ ]` → `[x]` in `docs/release-checklist.md` only when log is complete.

## 2. Authenticode

1. Copy `docs/release-signing.env.example` → set `CSC_LINK` / `CSC_KEY_PASSWORD` on release machine (or GitHub secrets + workflow `Release (signed, manual)`).
2. `npm run release:dist` (no `-AllowUnsigned`).
3. `signtool verify /pa "release\Byteback Setup 0.1.0.exe"` must succeed.
4. Record in `audit/PHASE4_SIGN_LOG.md`.
5. Then check Authenticode in `docs/release-checklist.md`.

## 3. NSIS clean-box smoke

1. Fresh Windows VM (no dev tools required for examiner test).
2. Lab build (unsigned OK for smoke, not for Authenticode PASS): `npm run release:dist:unsigned` → `release/Byteback Setup *.exe`. Signed path: step 2 + `npm run release:dist`. `npm run phase4:status` prints installer path + SHA-256 when the lab exe exists.
3. Install on the VM, launch app, confirm `byteback_engine.node` loads (scan-from-image smoke).
4. Record in `audit/PHASE4_VM_SMOKE_LOG.md` (installer SHA-256, VM id, PASS/FAIL).
5. Check NSIS row in checklist only after VM run — local exe build alone is not PASS.

## 4. Tag

1. Move `CHANGELOG.md` **Unreleased** bullets into a dated `## [x.y.z]` section (not Unreleased-only).
2. Bump `package.json` version to match the tag.
3. Tag `v1.0.0` only when **all** checklist gates including A/B/C and code sign are `[x]` with logs.
4. `phase4:status` treats CHANGELOG `[x]` without a released section as **DISHONEST**.

Do **not** mark the Cursor thread goal complete until this runbook is honestly done.

**Status command:** `npm run phase4:status` (exit 1 while checklist rows above stay `[ ]`). Vitest smoke (Windows): `src/shared/audit-phase4-gate.test.ts`.
