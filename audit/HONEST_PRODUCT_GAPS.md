# Honest product gaps

Companion to `AUDIT_REPORT.md` §8 and `docs/release-checklist.md`. Scope limits
stated plainly — updated after the parity round (Btrfs, signature expansion,
AES-KW). Last refresh: 2026-11-26.

## Blocks `v1.0.0` tag (checklist) — unchanged, operator-gated

| Gap | Why open | Evidence |
|-----|----------|----------|
| Field USB rows A/B/C | Needs physical NTFS HDD, FAT32 USB, exFAT SD; MD5 compare | `release-checklist.md` `[ ]` |
| Authenticode | Signing cert + secrets on release machine only | No `CSC_*` in repo |
| NSIS on clean VM | Installer smoke not run on fresh Windows | Checklist `[ ]` |
| Tagged release | Still `0.1.0`; `CHANGELOG.md` Unreleased only | `package.json` |

## Competitive / roadmap (not claimed today)

| Gap | Byteback today |
|-----|----------------|
| Streaming virtual-disk read | VHD/VHDX/VMDK/VDI/QCOW2/DMG sources supported with **64 MiB RAM expand cap**; no runtime attach-to-OS |
| Multi-pass sector imaging | ddrescue-style skip-ahead + trim pass + sidecar resume; Technician-class live hardware map still open |
| FileVault2 / APFS encrypted recovery | AES-KW primitive done; keybag→KEK→VEK pipeline in progress |
| ZFS / F2FS / UFS / JFS / ReiserFS / VMFS | Not implemented — roadmap items |
| Btrfs | **Implemented** (single-device translation, backup-root deleted recovery); multi-device RAID5/6 metadata honestly refused |
| Network agent, WinPE, BitLocker TPM-only | Out of v1 checklist |
| Signature breadth | ~230 curated (embedded + JSON); text-magic intentionally dropped; PhotoRec's ~480 not claimed |
| HTTP Range E01 in UI | Engine code exists; **IPC/file picker reject http**; dashboard hint |
| Full ReFS content recovery | Discovery-only (honest) — ministore parse is a roadmap item |

## Lab automation (done — not field)

| Check | Result |
|-------|--------|
| Native ctest | 970 passing locally (4 optional hardware/bench skips) |
| Vitest | 500/500 (`npm run verify:audit` subsets) |
| Playwright e2e | 45/45 |

Field USB and code signing remain the honest gate for `v1.0.0`.
