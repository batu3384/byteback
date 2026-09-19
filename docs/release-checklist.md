# Byteback 1.0 release checklist

This file is the gate for a `v1.0.0` tag. Unchecked items are **not done**.
Do not treat a green CI run as 1.0.

## Engine / examiner (lab)

- [x] Native GoogleTest Release suite (local `ctest -C Release`; skips documented)
- [x] Vitest renderer/shared
- [x] Typecheck (`tsconfig.web.json` + `tsconfig.node.json`)
- [x] Examiner scan-from-image (RAW/E01/VHD family, 64 MiB VHD expand cap)
- [ ] Full `npx playwright test` against a current `npm run build` (operator)

## Field USB (cannot be faked)

- [ ] Row A — NTFS HDD intern quick/deep
- [ ] Row B — FAT32 USB deleted photos, 5/5 MD5
- [ ] Row C — exFAT SD card

## Production hardening

- [ ] Authenticode code sign (5.1 — needs signing cert / user)
- [x] Auto-update **off** (no electron-updater; README Updates)
- [x] Local crash dumps only (`uploadToServer: false`)
- [ ] NSIS installer smoke on a clean Windows box
- [ ] CHANGELOG for the tagged version

## Out of v1 (do not block 1.0 on these)

- Btrfs / ZFS / F2FS / UFS parsers
- Mac native APFS
- Network agent, WinPE
- BitLocker TPM-only bypass
- ffmpeg WAV proxy (no new dependencies)

## Tag rule

`v1.0.0` only after field A/B/C PASS **and** code sign. Until then the product
version stays `0.1.x`. This checklist existing is not a 1.0 claim.
