# Byteback 1.0 release checklist

This file is the gate for a `v1.0.0` tag. Unchecked items are **not done**.
Do not treat a green CI run as 1.0.

Operator steps for open rows: [audit/PHASE4_OPERATOR_RUNBOOK.md](../audit/PHASE4_OPERATOR_RUNBOOK.md).

## Engine / examiner (lab)

- [x] Native GoogleTest Release suite (local `ctest -C Release`; skips documented)
- [x] Vitest renderer/shared
- [x] Typecheck (`tsconfig.web.json` + `tsconfig.node.json`)
- [x] Examiner scan-from-image (RAW/E01/VHD family, 64 MiB VHD expand cap)
- [x] Audit 2026-10-28 code hardening (ReDoS + IPC guard, WAL FULL, ErrorBoundary + session log, IPC split, `vhdx_source.cpp`, VHD fuzz, wipe serial re-read)
- [x] Playwright e2e (45 tests; CI Windows job + local verify 2026-10-28 — **not** field USB)
- [ ] Full operator sign-off on a fresh `npm run build` before tag (optional repeat)

## Field USB (cannot be faked)

Record results in [audit/FIELD_USB_LOG.md](../audit/FIELD_USB_LOG.md) when run.

- [ ] Row A — NTFS HDD intern quick/deep
- [ ] Row B — FAT32 USB deleted photos, 5/5 MD5
- [ ] Row C — exFAT SD card

## Production hardening

- [ ] Authenticode code sign — log: [audit/PHASE4_SIGN_LOG.md](../audit/PHASE4_SIGN_LOG.md) (`npm run release:dist` + `CSC_*`; `[ ]` until `signtool verify` PASS)
- [x] Auto-update **off** (no electron-updater; README Updates)
- [x] Local crash dumps only (`uploadToServer: false`)
- [ ] NSIS installer smoke on a clean Windows VM — log: [audit/PHASE4_VM_SMOKE_LOG.md](../audit/PHASE4_VM_SMOKE_LOG.md) (`npm run release:dist:unsigned`; VM run required)
- [ ] CHANGELOG for the tagged version (Unreleased section tracks 0.1.x audit work in [CHANGELOG.md](../CHANGELOG.md))

## Dev supply chain (packaging/CI machine)

- [x] CI gate: `npm run audit:prod` (`npm audit --omit=dev`)
- [x] CI gate: full `npm audit` (electron-vite 6 + vite 8 + vitest 4.1.12)

## Out of v1 (do not block 1.0 on these)

- Btrfs / ZFS / F2FS / UFS parsers
- Mac native APFS
- Network agent, WinPE
- BitLocker TPM-only bypass
- ffmpeg WAV proxy (no new dependencies)

## Tag rule

`v1.0.0` only after field A/B/C PASS **and** code sign. Until then the product
version stays `0.1.x`. This checklist existing is not a 1.0 claim.
