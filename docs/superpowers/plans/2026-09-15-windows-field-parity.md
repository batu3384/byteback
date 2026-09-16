# Windows Field Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lab golden for deleted FAT32 JPEG MD5, exFAT preservePaths, NTFS unalloc INDX names, and UTF-8 recovery dest — then field kit for rows B→C→A without claiming parity until USB 5/5.

**Architecture:** Fill deleted FAT/exFAT runs from first cluster when the FAT chain is shorter than size. Walk exFAT directories with a path stack. Recarve unallocated clusters for `parseIndxRecord` hits (`ntfs_i30_unalloc`). Write recovered bytes via `std::filesystem::u8path` / wide paths on Windows.

**Tech Stack:** C++17 native engine, gtest, existing volume fixtures, Electron examiner unchanged except source labels.

**Spec:** `docs/superpowers/specs/2026-09-15-windows-field-parity-design.md`

## Global Constraints

- No POSIX `openDrive`, no H.264 decode, no `$INDEX_BITMAP` as skip-filter, no Mac native / Disk Drill Mac claim, no BitLocker TPM bypass, no network agent, no WinPE.
- No new dependencies. TDD: failing test first. `OrphanIndxMagicDoesNotEmitI30` stays green.
- Unalloc INDX: `parseIndxRecord` + FILE_NAME only; raw `INDX` magic is not a hit.
- Field B/C/A PASS only from user media; agent ships procedure, does not invent PASS.
- PowerShell: `; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }`. No `git commit` unless asked.

---

### Task 1: Hygiene — competitive copy + honesty already in tree

**Files:**
- Modify: `docs/competitive-analysis-2026-09-05.md`
- Modify: `README.md` (no Mac parity sentence)
- Honesty `ioUnread` / `content_unread` / LIKE ESCAPE already in working tree — do not revert.

- [x] **Step 1: Drop “rakip seviyede”**

Replace the conclusion that NTFS/FAT/carve equals Disk Drill / R-Studio field parity with lab-vs-field language. Program 1 gates (USB B, unalloc INDX) stay open until golden+field pass.

- [ ] **Step 2: README**

State Windows examiner; APFS image/.E01 only; no Disk Drill Mac claim.

---

### Task 2: FAT32 5 deleted JPEG golden (watch RED)

**Files:**
- Modify: `native/tests/fixtures/volume_fixtures.h`
- Modify: `native/tests/test_recovery_golden.cpp`
- Modify: `native/src/fs/fat_parser.cpp`

**Interfaces:**
- Produces: `byteback::testfix::minimalValidJpeg(uint8_t tag)` and `buildFat32DeletedJpegVolume()` — FAT32 (`countOfClusters >= 65525`), five 0xE5 dirents, FAT clusters 0, JPEG payload ≥ 2 clusters each so first-cluster-only chain fails MD5.
- Consumes: `runGoldenPipeline`, `crypto::Md5`.

- [ ] **Step 1: Write the failing test** (no `fillDeletedFatRuns` yet)

Fixture: reserved 32, two FATs, root cluster 2, `sectorsPerCluster=1`, data clusters ≥ 65525 so `parseFAT` takes the FAT32 walk. Five 8.3 dirents `PHOTO1.JPG`… with first byte `0xE5`, first clusters 3+4, 5+6, … FAT entries for those clusters = 0. Each JPEG unique `tag` and size > 512 so `chainRuns` (first cluster then FAT=0 stop) recovers < 90% → `checkSizeConsistency` fails success today.

```cpp
TEST_F(GoldenRecoveryTest, Fat32FiveDeletedJpegMd5) {
    auto vol = testfix::buildFat32DeletedJpegVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(vol));
    auto jpeg = testfix::minimalValidJpeg(0x11);
    auto stats = runGoldenPipeline(reader, store_, dest_, "quick", [&](const FileRecord& fr) {
        return fr.source == "fat" && fr.status == 0 && fr.sizeBytes == jpeg.size();
    });
    EXPECT_EQ(stats.found, 5u);
    EXPECT_EQ(stats.recovered, 5u);
    // then MD5 each recovered file against the five source JPEGs
}
```

- [ ] **Step 2: Run test — expect RED**

```
cd native/build; ctest -C Release -R Fat32FiveDeletedJpegMd5 --output-on-failure
```

Expected: found may be 5 but recovered < 5, or MD5 mismatch (truncated first cluster). If already green, stop and re-read chain math — do not claim a fix that was not needed.

- [ ] **Step 3: Minimal implementation**

In `fat_parser.cpp` anonymous namespace:

```cpp
void fillDeletedFatRuns(FileRecord& fr, uint32_t firstCluster, uint64_t fileSize,
                        uint32_t sectorsPerCluster, uint64_t dataStartSector,
                        uint32_t bytesPerSector) {
    if (firstCluster < 2 || fileSize == 0 || sectorsPerCluster == 0 || bytesPerSector == 0) return;
    const uint64_t clusterBytes = static_cast<uint64_t>(sectorsPerCluster) * bytesPerSector;
    uint64_t nClus = (fileSize + clusterBytes - 1) / clusterBytes;
    if (nClus == 0) nClus = 1;
    if (nClus > (1u << 20)) nClus = 1u << 20;
    const uint64_t startSec = dataStartSector + static_cast<uint64_t>(firstCluster - 2) * sectorsPerCluster;
    fr.runs = {{startSec, nClus * sectorsPerCluster}};
    fr.startSector = startSec;
    fr.endSector = startSec + nClus * sectorsPerCluster;
}
```

Call after `buildRunsFromChain` when `deleted && fatRunBytes(fr.runs, bps) < fileSize`. Same for FAT16 root and FAT32 walk. Restore FAT32 `name[0]==0xE5` to `'_'` like FAT16.

- [ ] **Step 4: Re-run — expect GREEN** (`Fat32FiveDeletedJpegMd5` + existing FatQuick*)

---

### Task 3: exFAT deleted + preservePaths golden

**Files:**
- Modify: `native/tests/fixtures/volume_fixtures.h` — `buildExFatDeletedPreservePathsVolume()`
- Modify: `native/tests/test_recovery_golden.cpp`
- Modify: `native/src/fs/fat_parser.cpp` `parseExFAT`

**Interfaces:**
- Produces: root dir “pics” (in-use, cluster 3); deleted `shot.jpg` in that dir (0x05/0x40/0x41, cluster 4); `fr.path` such that `safeRelativeDir(fr.path)=="pics"`.
- Consumes: `fillDeletedFatRuns` from Task 2; `joinDestDir` + `safeRelativeDir` (already in `path_util`).

- [ ] **Step 1: Failing test**

```cpp
TEST_F(GoldenRecoveryTest, ExFatDeletedPreservePathsMd5) {
    auto vol = testfix::buildExFatDeletedPreservePathsVolume();
    // scan quick, pick shot.jpg status 0
    EXPECT_EQ(safeRelativeDir(hit.path), "pics");
    auto dest = joinDestDir(dest_, safeRelativeDir(hit.path));
    auto res = engine.recoverFile(reader, hit, dest);
    EXPECT_TRUE(res.success);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(dest) / "shot.jpg"));
}
```

Today `currentPath` stays `"/"` → `safeRelativeDir` empty → file lands in dest root. RED.

- [ ] **Step 2: Run — expect RED** (`safeRelativeDir` not `"pics"`)

- [ ] **Step 3: DirJob stack** in `parseExFAT`: `{cluster, path}`; on directory emit push `{firstCluster, currentPath + name + "/"}`; `fillDeletedFatRuns` when `!inUse && fatRunBytes < dataLength`.

- [ ] **Step 4: GREEN**

---

### Task 4: NTFS unalloc INDX recarve

**Files:**
- Modify: `native/include/scan/discovery_sources.h` — add `ntfs_i30_unalloc`
- Modify: `src/shared/source-label.ts` + tests + `src/renderer/i18n/index.ts`
- Modify: `native/tests/fixtures/volume_fixtures.h` — `buildNtfsUnallocIndxVolume()`
- Modify: `native/tests/test_ntfs_parser.cpp`
- Modify: `native/src/fs/ntfs_parser.cpp`

**Interfaces:**
- Consumes: `ntfs::parseIndxRecord`, `buildUnallocatedRanges(Ntfs, ...)`
- Produces: discovery source `ntfs_i30_unalloc` (same recoverability as `ntfs_i30`: false)

- [ ] **Step 1: Failing test**

Volume = index-allocation fixture resized so cluster 10 holds a second valid INDX (`unalloc_only.txt`, child MFT 88) **not** in Dir `$I30` (cluster 3).

```cpp
TEST(NtfsParser, UnallocIndxEmitsNameFromFreeCluster) {
    auto img = byteback::testfix::buildNtfsUnallocIndxVolume();
    // scanAt carveOrphanMft false
    EXPECT_TRUE(saw Unalloc name with source ntfs_i30_unalloc);
}
// OrphanIndxMagicDoesNotEmitI30 unchanged
```

- [ ] **Step 2: RED** — no `ntfs_i30_unalloc` hit

- [ ] **Step 3: After MFT `$I30` runs**, skip owned sectors; walk `$Bitmap` free ranges, or if empty: ponytail cap 65536 clusters. `parseIndxRecord`; empty hints → skip (raw magic). Emit `ntfs_i30_unalloc`.

- [ ] **Step 4: GREEN** including `OrphanIndxMagicDoesNotEmitI30`

---

### Task 5: Recovery UTF-8 destination

**Files:**
- Modify: `native/include/recovery/path_util.h`, `native/src/recovery/path_util.cpp`
- Modify: `native/src/recovery/recovery_engine.cpp`, `native/src/recovery/validation.cpp`
- Modify: `native/tests/test_recovery_engine.cpp`, `native/tests/test_path_util.cpp`

**Interfaces:**
- Produces: `utf8Path(const std::string&)`, `pathToUtf8(const std::filesystem::path&)`
- `uniqueDestPath` / `joinDestDir` / `destDirIsSafe` / `ofstream` / `create_directories` use `utf8Path`

- [ ] **Step 1: Failing test** — dest dir name CJK `byteback_文` (not in CP1254). Recover resident `doc.txt`. `exists(dest / "doc.txt")` false today because `path(utf8)` is ACP.

- [ ] **Step 2: RED**

- [ ] **Step 3: `u8path` on Win32; `ofstream(utf8Path(destPath))`**

- [ ] **Step 4: GREEN**

---

### Task 6: Field B then C then A kit

**Files:**
- Create: `docs/field-test-2026-09/row-b/README.md`, `RESULT.md`
- Create: `docs/field-test-2026-09/row-c/README.md`, `RESULT.md`
- Create: `docs/field-test-2026-09/row-a/README.md`, `RESULT.md`
- Modify: `docs/field-test-protocol.md`, `docs/field-test-2026-09/README.md`

DURUM stays `bekliyor` until the user fills RESULT. Agent does not write PASS. Gate: B 5/5 MD5 or triage, no parity claim.

---

## Spec coverage

| Spec ölçüt | Task |
|---|---|
| FAT32 5 JPEG MD5 | 2 |
| exFAT preservePaths | 3 |
| unalloc INDX + no raw magic FP | 4 |
| Unicode dest | 5 |
| Field B→C→A | 6 |
| No Mac claim / competitive honesty | 1 |
| `$INDEX_BITMAP` skip yasak | 4 (bitmap only for free ranges, not skip-filter) |
