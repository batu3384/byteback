# Honest product gaps (audit 2026-10-28)

Companion to `AUDIT_REPORT.md` §8 and `docs/release-checklist.md`. **Not bugs to hide** — scope limits and v1.0 blockers stated plainly.

## Blocks `v1.0.0` tag (checklist)

| Gap | Why open | Evidence |
|-----|----------|----------|
| Field USB rows A/B/C | Needs physical NTFS HDD, FAT32 USB, exFAT SD; MD5 compare | `release-checklist.md` `[ ]` |
| Authenticode | Signing cert + secrets on release machine only | No `CSC_*` in repo; see `docs/release-signing.env.example` |
| NSIS on clean VM | Installer smoke not run on fresh Windows | Checklist `[ ]` |
| Tagged release | Still `0.1.0`; `CHANGELOG.md` Unreleased only | `package.json` |

## Competitive / roadmap (not claimed today)

From audit §8 — do **not** market as parity with Disk Drill / R-Studio Technician:

| Gap | Byteback today |
|-----|----------------|
| Streaming virtual-disk read | VHD family loads with **64 MiB RAM expand cap**; no runtime attach-to-OS |
| Multi-pass sector imaging + live map | RAW/E01 with MD5; not Technician-class runtime imaging |
| Btrfs / UFS parsers | Out of v1 checklist |
| Network agent, WinPE, BitLocker TPM-only | Out of v1 checklist |
| ~400 signature broşür | ~150 curated; text-magic intentionally dropped |
| HTTP Range E01 in UI | Engine code exists; **IPC/file picker reject http**; dashboard hint (`dash.imageScanHint`) |

## Lab automation (done — not field)

| Check | Result |
|-------|--------|
| Playwright e2e | 45 passed locally 2026-10-28; CI `build-windows` job runs `npm run test:e2e` |
| Vitest | 490 unit tests (`npm run verify:audit`) |
| Native ctest | Windows CI Release suite |

Field USB and code signing remain the honest gate for `v1.0.0`.
