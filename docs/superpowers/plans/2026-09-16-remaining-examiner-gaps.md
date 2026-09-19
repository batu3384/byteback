# Remaining Examiner Gaps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close remaining examiner gaps that are in-scope after adversarial review and Program 1: disk-wide hex/text search, MFT record viewer, VHD/VHDX ByteSource mount, jbd2 journal names, unalloc INDX Extra Found content via carve — without claiming Disk Drill / R-Studio parity and without Mac native, network agent, WinPE, or BitLocker TPM bypass.

**Architecture:** Reuse DiskReader + existing IPC/honesty patterns. Native first (gtest RED), then a thin N-API/IPC, then examiner UI matching HexEditor/ResultsView. No new dependencies.

**Tech Stack:** C++17 native, gtest, Electron/React examiner, vitest for TS.

**Spec:** `docs/competitive-analysis-2026-09-05.md` (open table) + this plan. Field USB A/B/C remain user-hardware gates.

## Global Constraints

- No POSIX `openDrive`, no H.264 decode, no `$INDEX_BITMAP` skip-filter, no Mac native / Disk Drill Mac claim, no BitLocker TPM bypass, no network agent, no WinPE.
- No new dependencies. TDD: failing test first. PowerShell: `; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }`.
- No `git commit` unless asked.
- Do not mark kurumsal / Faz 0.4 / this goal complete until every in-scope task is evidenced.
- Unalloc INDX Extra Found stays discovery-only unless a real `$DATA` run or carve payload is bound.

---

### Task 1: Disk-wide hex/text search (R-Studio hex find)

**Files:**
- Create: `native/include/search/hex_search.h`
- Create: `native/src/search/hex_search.cpp`
- Create: `native/tests/test_hex_search.cpp`
- Modify: `native/CMakeLists.txt` (add cpp to both library lists + test)
- Modify: `src/main/ipc-handlers.ts`, `src/shared/types.ts`, `src/preload/index.ts`
- Modify: `src/renderer/components/HexEditor/HexEditor.tsx` + i18n keys

**Interfaces:**
- Produces: `byteback::searchRawBytes(DiskReader&, const uint8_t* needle, size_t n, uint64_t maxHits, std::atomic<bool>*, bool* unread) -> std::vector<HexSearchHit>`
- `HexSearchHit { uint64_t byteOffset; }`
- Caps: needle 1..64 bytes, maxHits 256, 1 MiB chunks, overlap `n-1`. I/O fail sets `*unread` and does not invent hits from zeros.

- [ ] **Step 1: Write the failing test**

Memory volume with unique 4-byte needle at offset 1000; `searchRawBytes` must return that offset; unread fault in the window must set unread and not emit a padded-zero false hit.

- [ ] **Step 2: Run RED**

`cmake --build native/build --config Release --target byteback_tests ; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; ctest --test-dir native/build -C Release -R HexSearch --output-on-failure`

Expected: test binary missing symbol or FAIL no hits.

- [ ] **Step 3: Minimal native search**

Chunked `readSectors` + `std::search` with overlap. Skip `ioUnread` windows (set unread).

- [ ] **Step 4: GREEN native**

Same ctest filter, PASS. Keep `Fat32FiveDeletedJpegMd5` green if touched.

- [ ] **Step 5: IPC + HexEditor**

`searchHex` IPC: driveIndex, needle (hex string or latin1 text), maxHits. HexEditor: query box, hit list, click jumps sector. Vitest or existing Hex tests if present. i18n `hex.search`, `hex.noHits`, `hex.searchUnread`.

---

### Task 2: NTFS MFT record viewer (R-Studio/DMDE)

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp` / new `native/src/fs/mft_record_view.cpp`
- Create: `native/tests/test_mft_record_view.cpp`
- Modify: IPC + a ResultsView or Hex-adjacent panel

**Interfaces:**
- Produces: `MftRecordView { uint64_t mftRef; std::string signature; uint16_t flags; std::vector<MftAttrSummary> attrs; }`
- `getMftRecordView(DiskReader&, uint64_t mftRef) -> optional<MftRecordView>`
- Discovery-only dump; not a recover path. Unread MFT byte range emits existing `ntfs_mft_unread`, does not fake FILE.

- [ ] Failing test on `buildNtfsIndexAllocationVolume` record 0 (`FILE`, in-use, `$DATA` non-resident).
- [ ] Implement parse of one 1024-byte record into attr type/name/resident flag.
- [ ] IPC `getMftRecord` + examiner panel showing hex + attr list for selected NTFS hit’s MFT ref if present.

---

### Task 3: VHD/VHDX ByteSource (mount, not just carve signature)

**Files:**
- Create: `native/src/io/vhd_source.cpp` + header
- Create: `native/tests/test_vhd_source.cpp`
- Modify: `DiskReader` attach + UI open-image picker

**Interfaces:**
- Footer `conectix` VHD fixed disk: parse cookie, disk size, data offset; `read` maps to image file.
- Dynamic VHD: BAT lookup; sparse missing blocks return zeros with `paddedZeros` only if the spec says unallocated (document). Do not claim VHDX until a second test.
- No new deps. Cap: fixed VHD first.

- [ ] Fixture: tiny fixed VHD wrapping existing FAT16 superfloppy bytes.
- [ ] `attachVhd` + FATParser lists `TEST.TXT`.
- [ ] Wire open-image in examiner if an image-open path already exists; else CLI/bridge only this task.

---

### Task 4: jbd2 / ext3 journal names

**Files:**
- Modify: `native/src/fs/ext4_parser.cpp` or `native/src/fs/jbd2.cpp`
- Create: `native/tests/test_jbd2.cpp`

**Interfaces:**
- Walk compatible journal blocks for revoked/deleted inode names when journal present.
- Unread journal: existing honesty sentinel, do not parse zeros as names.

- [ ] Fixture with a journal descriptor + one name the inode table no longer has.
- [ ] Emit `ext4_journal` discovery or boost deleted ext4 dirent confidence.
- [ ] Keep current ext4 dirent tests green.

---

### Task 5: Unalloc INDX Extra Found content (carve bind)

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp` unalloc emit
- Modify: `native/tests/test_ntfs_parser.cpp`

**Interfaces:**
- If child MFT unseen: keep name-only `ntfs_i30_unalloc`.
- Optional: if a carver hit at a nearby unalloc cluster matches the FILE_NAME size/extension, copy runs onto a **separate** recoverable source (`ntfs_i30_carve`) — never steal live MFT `$DATA`.
- Honesty banner for name-only stays.

- [ ] Fixture: unalloc INDX name + JPEG in free cluster not in MFT.
- [ ] Recoverable hit MD5 equals JPEG; live `Dir` skip test stays green.

---

### Task 6: Out of v1 (document only, do not implement)

- Btrfs/ZFS/F2FS/UFS parsers
- Mac native APFS, Recovery Vault, iOS
- Network agent, WinPE
- BitLocker TPM-only bypass
- Camera RAW session Mac↔Win

Keep competitive table honest. Field A/B/C PASS only with user USB.

---

## Verification (every task)

```
cmake --build native/build --config Release --target byteback_tests ; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir native/build -C Release --output-on-failure
npx vitest run src/shared/scan-honesty.test.ts src/shared/source-label.test.ts src/renderer/i18n/index.test.ts
```

Do not claim Disk Drill / R-Studio parity. Goal remains open until Tasks 1–5 are evidenced or the user pauses.
