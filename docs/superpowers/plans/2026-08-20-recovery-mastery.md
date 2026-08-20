# Byteback — Scan & Recovery Mastery Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring Byteback closer to commercial data recovery software (R-Studio, Recuva, Disk Drill, PhotoRec); primary focus on disk scanning, breadth of deleted-data discovery, and recovery capability.

**Architecture:** Extend the existing `ScanCoordinator` → FS parsers → `CarvingEngine` → `RecoveryEngine` pipeline; layer improvements to scan scope (partition/volume/unallocated), metadata depth (NTFS path rebuild, ReFS), carving breadth (signatures + custom parsers), and recovery quality (preview, validation, dedup).

**Tech Stack:** C++17 native engine, Electron/React UI, SQLite metadata, GoogleTest + Vitest + Playwright.

**Spec:** This plan is derived directly from user requirements; a separate spec file `docs/superpowers/specs/2026-08-20-recovery-mastery-design.md` will be written after approval.

## Global Constraints

- Windows-only native I/O (`\\.\PhysicalDriveN`, `DeviceIoControl`).
- Engine reads source disk with `GENERIC_READ`; writes only to user-specified destination paths.
- No BitLocker bypass; FVEK or recovery password required.
- Post-TRIM SSD recovery is physically impossible — UI must state this clearly.
- No GPU/CUDA PFAC in early phases; CPU parallelism takes priority.
- Each phase must end with `ctest -C Release` + `npm run test` green.

---

## Current State Summary (August 2026)

### Strengths (preserve)

- NTFS: MFT run walk, deleted records (`status=0`), orphan FILE carve (deep), USN timeline, INDX slack, $LogFile hints, LZNT1 recovery.
- FAT/exFAT: chain walk, deleted 0xE5, VFAT.
- ext4: extent tree, deleted inode/dirent.
- Carving: Aho-Corasick ~114 signatures, FOV validator, BGC (sector-stepped, budgeted).
- BitLocker FVEK + clear-key recovery password, VSS, RAID 0/1/5/6/10.
- Audit SHA-256 chain tested; bad-sector telemetry wired to UI.

### Critical gaps (vs. commercial tools)

| Area | Byteback today | R-Studio / PhotoRec / Recuva |
|------|----------------|------------------------------|
| Scan target | Entire physical disk | Partition / volume / unallocated selection |
| Drive letter (D:) | None | Available |
| Carve scope | All disk sectors | Mostly free space / unallocated |
| Signature count | ~114 | 300–400+ |
| NTFS path rebuild | Partial (MFT parent walk) | Full directory tree + $LogFile |
| ReFS | None | Available |
| Parallel scan | Single thread | Multi-core |
| Preview | None | Image/PDF/text |
| Dedup | None | MFT+carve merge |
| Office/SQLite/Video | None | Custom parsers |
| E2E recovery test | Smoke test | Scenario tests |

---

## Phase 0 — Scan Infrastructure & UX Foundation (P0)

**Objective:** User can target a unit like D: correctly; carving does not scan the entire disk unnecessarily.

### Task 0.1: Volume / partition-targeted scan

**Files:**
- Modify: `native/include/scan_coordinator.h`
- Modify: `native/src/scan_coordinator.cpp`
- Modify: `native/src/bridge/bridge_scan.cpp`
- Modify: `src/main/ipc-handlers.ts`, `src/preload/index.ts`, `src/shared/types.ts`
- Modify: `src/renderer/components/Dashboard/DriveCard.tsx`, `src/renderer/App.tsx`

**Interfaces:**
- Consumes: existing `runQuickScan`, `runDeepScan`, `runCarveScan`
- Produces:
  ```cpp
  struct ScanTarget {
      int driveIndex;
      int64_t partitionStartSector = -1; // -1 = entire disk
      uint64_t partitionSizeSectors = 0;
      bool carveUnallocatedOnly = false;
  };
  void ScanCoordinator::startScan(const ScanTarget& target, const std::string& scanType, ...);
  ```

- [ ] **Step 1:** Add `ScanTarget` struct + `DiskReader::setReadBounds(startSector, sectorCount)` (read clamp).
- [ ] **Step 2:** Limit `runQuickScan` by partition offset/size; carve stays within same bounds.
- [ ] **Step 3:** NAPI `startScan(driveIndex, scanType, opts?)` — `partitionIndex` or `startSector`+`sizeSectors`.
- [ ] **Step 4:** UI: use `listPartitions` API in DriveCard; partition selector dropdown.
- [ ] **Step 5:** Test: only the 2nd partition is scanned on a synthetic GPT image.

### Task 0.2: Drive letter → physical disk mapping

**Files:**
- Create: `native/src/io/volume_mapper_win.cpp`, `native/include/io/volume_mapper_win.h`
- Modify: `native/src/bridge/bridge_drives.cpp`

**Interfaces:**
- Produces: `std::optional<ScanTarget> resolveDriveLetter(const std::wstring& letter); // e.g. L"D:"`

- [ ] **Step 1:** Map letter → PhysicalDrive + offset via `QueryDosDeviceW` + `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`.
- [ ] **Step 2:** NAPI `resolveVolume("D:")` → `{ driveIndex, startSector, sizeSectors, fsType }`.
- [ ] **Step 3:** UI: "Scan D:" quick shortcut on Dashboard.
- [ ] **Step 4:** Test: mock IOCTL or integration (if a volume exists in test environment).

### Task 0.3: Unallocated-only carve mode

**Files:**
- Create: `native/src/fs/unallocated_map.cpp`, `native/include/fs/unallocated_map.h`
- Modify: `native/src/scan_coordinator.cpp`, `native/src/carver/signature_engine.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct SectorRange { uint64_t start; uint64_t count; };
  std::vector<SectorRange> buildUnallocatedRanges(DiskReader&, VolumeFsKind, uint64_t volOffset, uint64_t volSize);
  ```

- [ ] **Step 1:** NTFS: read `$Bitmap` → free cluster → sector ranges.
- [ ] **Step 2:** FAT: sector ranges outside allocated cluster chains from FAT table.
- [ ] **Step 3:** `CarvingEngine::scanRanges(reader, ranges, ...)` — refactor existing `scan`.
- [ ] **Step 4:** Deep scan default: metadata full partition + carve unallocated only.
- [ ] **Step 5:** Test: no carve candidates produced in allocated region on NTFS image.

### Task 0.4: Scan profiles & honest labels

**Files:**
- Modify: `src/renderer/components/ScanView/ScanView.tsx`, `DriveCard.tsx`

- [ ] **Step 1:** Three modes: **Quick** (metadata, orphan off), **Deep** (metadata + unallocated carve), **Full Disk Carve** (legacy behavior, with warning).
- [ ] **Step 2:** SSD detected → TRIM warning modal before scan starts.
- [ ] **Step 3:** Playwright: mode selection + warning text smoke.

**Phase 0 exit criteria:** Deep scan via D: letter or partition selection; carve only free space; tests green.

---

## Phase 1 — NTFS Metadata Depth (P0)

**Objective:** Bring deleted-file name + folder path ratio closer to R-Studio.

### Task 1.1: Full $MFT coverage

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`

- [ ] **Step 1:** In addition to boot `$MFT` run walk, scan all MFT extents via `$MFT::$DATA` attribute runs.
- [ ] **Step 2:** Synthetic test image for MFT fragmentation (non-contiguous $MFT).
- [ ] **Step 3:** Merge records overlapping with orphan carve by MFT record number.

### Task 1.2: Directory tree rebuild

**Files:**
- Create: `native/src/fs/ntfs_path_rebuild.cpp`, `native/include/fs/ntfs_path_rebuild.h`
- Modify: `native/src/fs/ntfs_parser.cpp`

**Interfaces:**
- Produces: `std::string rebuildPath(uint64_t mftRecord, const MftIndex& idx);`

- [ ] **Step 1:** Collect parent MFT ref + name from all FILE records.
- [ ] **Step 2:** For deleted parents, attach `$LogFile` + INDX slack hints at low confidence score.
- [ ] **Step 3:** Populate `fr.path` and `fr.name` as full path (`/Users/foo/bar.jpg`).
- [ ] **Step 4:** Test: deleted subdirectory + file scenario.

### Task 1.3: $LogFile → recoverable record promotion

**Files:**
- Modify: `native/src/fs/ntfs_logfile.cpp`, `ntfs_parser.cpp`

- [ ] **Step 1:** Match names from LogFile to MFT record ID (LSN/transaction hint).
- [ ] **Step 2:** Raise `confidence` on matched records; UI label "LogFile verified".
- [ ] **Step 3:** Test: extend `test_ntfs_logfile.cpp`.

### Task 1.4: USN → deletion event filtering

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`, `src/renderer/components/ResultsView/ResultsView.tsx`

- [ ] **Step 1:** Keep USN records in timeline; do not mix into separate "recoverable" list.
- [ ] **Step 2:** ResultsView "Deleted only" filter (`status === 0`).
- [ ] **Step 3:** Test: Vitest filter unit.

**Phase 1 exit criteria:** ≥80% full path on NTFS deleted files (synthetic golden image set).

---

## Phase 2 — Carving Breadth & Speed (P1)

**Objective:** Format coverage and acceptable speed approaching PhotoRec level.

### Task 2.1: Signature library expansion

**Files:**
- Modify: `native/src/carver/signature_engine.cpp` (embedded table)
- Create: `resources/signatures-extended.json`
- Create: `native/tests/test_signature_coverage.cpp`

- [ ] **Step 1:** Add 200+ new signatures from PhotoRec / file(1) magic list (priority: jpg, png, mp4, mov, docx, xlsx, pptx, zip, rar, 7z, pdf, sqlite, pst, eml, wav, mp3, heic, cr2, nef, orf, arw).
- [ ] **Step 2:** Define FOV validator or footer requirement for each signature.
- [ ] **Step 3:** Test: known magic → correct extension.

### Task 2.2: Custom structural parsers

**Files:**
- Create: `native/src/carver/parsers/zip_family.cpp`, `sqlite_carver.cpp`, `mp4_carver.cpp`
- Modify: `native/src/carver/signature_engine.cpp`

- [ ] **Step 1:** ZIP-based Office (docx/xlsx/pptx): central directory scan.
- [ ] **Step 2:** SQLite: header + schema page validation, size estimation.
- [ ] **Step 3:** MP4/MOV: `ftyp` + `moov`/`mdat` atom walk; tail search if moov at end.
- [ ] **Step 4:** Test: minimal binary fixture per parser.

### Task 2.3: CPU parallel carve

**Files:**
- Modify: `native/src/carver/signature_engine.cpp`
- Create: `native/include/util/thread_pool.h` (minimal, ponytail: std::async or fixed pool)

- [ ] **Step 1:** Split chunk list across N workers; merge results with mutex.
- [ ] **Step 2:** `std::atomic` progress aggregation.
- [ ] **Step 3:** Benchmark test: 1 GB synthetic image, 4 thread vs 1 thread.

### Task 2.4: MFT ↔ carve deduplication

**Files:**
- Create: `native/src/scan/dedup_index.cpp`
- Modify: `native/src/bridge/bridge_scan.cpp`, `native/src/db/metadata_store.cpp`

- [ ] **Step 1:** Detect carve/MFT overlap via `(startSector, size, hash prefix)`.
- [ ] **Step 2:** Keep high-confidence MFT record; mark carve copy with `duplicate_of`.
- [ ] **Step 3:** UI: "Hide duplicates" toggle.

**Phase 2 exit criteria:** 250+ signatures; ≥2× speed on 4 cores; Office/SQLite/MP4 carve working.

---

## Phase 3 — Other File Systems (P1)

### Task 3.1: ReFS parser (Windows Server / Win10+)

**Files:**
- Create: `native/src/fs/refs_parser.cpp`, `native/include/fs/refs_parser.h`
- Modify: `native/src/fs/partition_scanner.cpp`, `scan_coordinator.cpp`

- [ ] **Step 1:** ReFS superblock + container table recognition.
- [ ] **Step 2:** Directory B+ tree walk, deleted entry.
- [ ] **Step 3:** Test: ReFS test image (or minimal fixture).

### Task 3.2: exFAT deleted entry improvement

**Files:**
- Modify: `native/src/fs/fat_parser.cpp`

- [ ] **Step 1:** Tolerance for entry-set fragmentation (non-contiguous name entries).
- [ ] **Step 2:** Scan directories with mixed deleted + allocated entries.

### Task 3.3: APFS catalog depth

**Files:**
- Modify: `native/src/fs/apfs_container.cpp`, `apfs_parser.cpp`

- [ ] **Step 1:** Snapshot tree walk (remove documented limit).
- [ ] **Step 2:** Increase `runs` fill rate from `apfs_file` discovery.
- [ ] **Step 3:** Test: extend existing APFS fixture.

**Phase 3 exit criteria:** ReFS volume recognized and files listed; APFS recoverable file ratio improved.

---

## Phase 4 — Recovery Quality & Validation (P1)

### Task 4.1: Recovery preview

**Files:**
- Create: `native/src/recovery/preview_reader.cpp`
- Modify: `src/renderer/components/ResultsView/ResultsView.tsx`
- Modify: `src/main/ipc-handlers.ts`

- [ ] **Step 1:** First 64 KB read NAPI `readFilePreview(scanId, fileId)`.
- [ ] **Step 2:** UI: image thumbnail (jpg/png/gif), text hex+ascii, PDF first page (simple).
- [ ] **Step 3:** Test: Vitest preview API mock.

### Task 4.2: Post-recovery validation

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`

- [ ] **Step 1:** Re-run validator on recovered file (carve-sourced).
- [ ] **Step 2:** `RecoveryResult.validationScore` + `validationError`.
- [ ] **Step 3:** UI: "Corrupt / Complete" column.

### Task 4.3: Fragment assembly (multi-run)

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`, `bgc.cpp`

- [ ] **Step 1:** Write BGC `frag1 + gap + frag2` output to `runs` vector (partially exists — complete).
- [ ] **Step 2:** Limited brute for 3+ fragments (ponytail: max 3 gap, budget).
- [ ] **Step 3:** Test: two-fragment JPEG golden.

### Task 4.4: BitLocker expansion (password protector)

**Files:**
- Modify: `native/src/fs/bitlocker_unlock.cpp`

- [ ] **Step 1:** Password protector (VMK decrypt) — libmsbde or minimal PBKDF2 implementation.
- [ ] **Step 2:** Keep explicit error message for TPM/startup-key.
- [ ] **Step 3:** Test: known test vector (Microsoft documentation).

**Phase 4 exit criteria:** User previews before recovery; corrupt file ratio reported.

---

## Phase 5 — Reliability, Test & Benchmark (P2)

### Task 5.1: Golden image regression suite

**Files:**
- Create: `native/tests/fixtures/ntfs_deleted/`, `native/tests/test_recovery_golden.cpp`
- Create: `scripts/generate-fixtures.ps1`

- [ ] **Step 1:** NTFS: 10 deleted files (txt, jpg, docx, fragmented jpg).
- [ ] **Step 2:** FAT32: 5 deleted files.
- [ ] **Step 3:** CI: `ctest -R GoldenRecovery` — found / recovered ratio report.

### Task 5.2: E2E recovery flow

**Files:**
- Modify: `e2e/electron-smoke.spec.ts`
- Create: `e2e/recovery-flow.spec.ts`

- [ ] **Step 1:** Mock native or test disk: scan → result → recover.
- [ ] **Step 2:** MD5 verification.

### Task 5.3: Scan checkpoint / resume

**Files:**
- Modify: `native/src/scan_coordinator.cpp`, `metadata_store.cpp`

- [ ] **Step 1:** Write last scanned sector to SQLite.
- [ ] **Step 2:** Resume interrupted scan via "Continue".
- [ ] **Step 3:** Test: interrupt mid-scan + resume.

### Task 5.4: Performance profile

**Files:**
- Create: `native/tests/bench_scan.cpp` (gtest benchmark or simple stopwatch)

- [ ] **Step 1:** 500 GB simulated (sparse file) deep scan duration report.
- [ ] **Step 2:** Honest benchmark table in README.

**Phase 5 exit criteria:** Golden tests in CI; E2E recovery pass; resume working.

---

## Phase 6 — Advanced (P3, optional)

| Task | Description | Note |
|------|-------------|------|
| 6.1 GPU PFAC | CUDA/OpenCL Aho-Corasick | Large investment; if Phase 2 parallel CPU insufficient |
| 6.2 ReFS integrity stream | ReFS checksum validation | Server scenario |
| 6.3 Cloud / network image | E01 over network | Forensic scenario |
| 6.4 macOS / Linux I/O | `disk_reader_posix.cpp` | Platform expansion |
| 6.5 AI file classification | Entropy + ML category | Not marketing; real value uncertain |

---

## Priority Order (Implementation Sequence)

```
Phase 0 (P0) → Phase 1 (P0) → Phase 2 (P1) → Phase 4.1-4.3 (P1) → Phase 3 (P1) → Phase 4.4 (P2) → Phase 5 (P2) → Phase 6 (P3)
```

**Rationale:** Correct target (D:/partition) + unallocated carve first → NTFS path → format breadth → recovery quality → other FS → test harness.

---

## Success Metrics (commercial comparison)

| Metric | Today (estimated) | Target (after Phases 0–5) |
|--------|-------------------|---------------------------|
| NTFS deleted file discovery (HDD, recent delete) | ~70% | ≥90% |
| NTFS with full path | ~40% | ≥75% |
| Carve format count | 114 | ≥250 |
| Deep scan time (500GB HDD) | baseline | ≤0.5× (parallel + unallocated) |
| Recovery validation | MD5 only | MD5 + structural validator |
| Partition targeting | None | Available |
| ReFS | None | Basic |

---

## Risks & Conscious Limits

1. **TRIM/SSD:** Cannot be overcome in software; warning only, plus early-delete scenario.
2. **Overwrite:** No tool can recover overwritten data.
3. **BitLocker TPM:** Hardware-bound; support beyond password/FVEK may remain limited.
4. **Full APFS:** Apple ecosystem is complex; 100% R-Studio Mac expectation is unrealistic.
5. **Legal:** Recovery tool only; user must own data and have permission — remind in UI.

---

## First Sprint Recommendation (after approval)

1. Task 0.1 — partition-targeted scan
2. Task 0.3 — unallocated-only carve
3. Task 0.2 — D: letter mapping
4. Task 1.2 — NTFS path rebuild

These four tasks have the most direct impact on the user's "find deleted data on D: drive" scenario.
