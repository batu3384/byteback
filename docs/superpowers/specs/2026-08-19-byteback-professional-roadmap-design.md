# Byteback — Professional Roadmap Design

**Date:** 2026-08-19  
**Status:** Approved (depth-first Windows forensic)  
**Scope:** byteback native engine + Electron UI

## Goal

Bring Byteback from a capable prototype to a professional forensic recovery tool competitive with X-Ways / R-Studio on Windows-first workflows, without speculative features.

## Strategy

**Depth-first Windows Forensic** — finish reliability and NTFS/VSS depth before Apple FS or GPU carving.

## Phases

| Phase | Focus | Exit criteria |
|-------|-------|---------------|
| **0** | Reliability & trust | E2E scan→recover test, bounded reads, honest UI state, carving FOV for noisy sigs |
| **1** | NTFS depth | `$LogFile` v2 hints, deleted MFT scoring, USN report |
| **2** | VSS | Snapshot mount + scan path |
| **3** | RAID & batch | Virtual RAID → scan/recover, BGC polish, batch export |
| **4** | Content search | Async grep, content FTS, DB-side filters |
| **5** | Apple FS | HFS overflow, APFS container walk |
| **6** | Ops | NSRL, E01 CI cross-check, case mgmt, fuzzing |

## Global Constraints

- C++17, CMake 3.20+, Node NAPI v8 addon
- Windows primary; non-Windows builds must compile (VSS/NTFS IOCTL gated)
- No new runtime deps without explicit approval
- Metadata in SQLite; audit log hash-chained SHA-256
- Tests: `ctest -C Release` must pass before merge
- Conscious limits documented in README (BitLocker decrypt, full APFS, GPU PFAC, etc.)

## Phase 0 — Reliability (current sprint)

1. **E2E integration test** — FAT fixture → `runQuickScan` → `MetadataStore` → `RecoveryEngine` → content + MD5
2. **ReadSectors cap** — max 1 MiB per NAPI call (done)
3. **Dashboard scan state** — resume banner only for `status === 0` (running)
4. **MPEG-TS FOV** — structural sync-byte validation at 188-byte packet boundaries
5. **VSS metadata** — `createdAt` from volume object creation time (Windows)

## Out of scope (this sprint)

- Full `$LogFile` redo replay
- VSS mount/parse
- BitLocker decrypt
- NSRL / GPU PFAC

## Success metrics

- Native tests ≥ 124 (was 123) after E2E + validator tests
- Zero false "active scan" banners after completed scans
- MPEG-TS validator rejects single-packet false positives
