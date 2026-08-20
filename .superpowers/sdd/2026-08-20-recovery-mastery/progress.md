# SDD ledger — plan: wolf-recovery/docs/superpowers/plans/2026-08-20-recovery-mastery.md

**BASE:** 3bd79733a03fa616a1dff17936635ad9f96819fa
**Branch:** feat/recovery-mastery

## Preflight scan

| Tasks | Shared interface | Finding | Ruling |
|-------|------------------|---------|--------|
| 0.1 / 0.3 | `ScanTarget`, `runCarveScan` ranges | 0.3 adds unallocated-only; 0.1 adds bounds | 0.1 uses sector bounds only; 0.3 adds unallocated filter later |
| 0.1 / 0.2 | `ScanTarget` | Both extend scan target | 0.2 adds drive letter resolver; 0.1 uses partition fields |
| 1.2 / 0.1 | `FileRecord.path` | Path rebuild needs partition scan | 0.1 first — no conflict |

## Tasks

- Task 0.1: complete (partition-targeted scan — native ScanTarget/ScanBounds, scanRange carve, NAPI opts, UI dropdown)
- Task 0.2: complete (volume_mapper_win, resolveVolume/listVolumeLetters NAPI, Dashboard shortcut)
- Task 0.3: complete (unallocated_map NTFS $Bitmap + FAT, deep scan unallocated-only carve)
- Task 0.4: complete (scan profiles quick/deep/full_carve, SsdTrimModal, Playwright smoke)
- Task 1.1: complete (MFT multi-extent, fragmented test, dedup by MFT record)
- Task 1.2: complete (ntfs_path_rebuild, RebuildsDeletedFilePath test)
- Task 1.3: complete (NtfsLogHintCollector, confidence boost, ntfs_mft_logfile source)
- Task 1.4: complete (USN timeline only, ResultsView deleted filter + isRecoverableListSource)
- **Tests:** 210/210 native (2 skipped), typecheck + vitest green
- Task 2.1: complete (embedded+JSON merge, signatures-extended.json ~247 sigs, test_signature_coverage)
- Task 2.2: complete (zip_family, sqlite_carver, mp4_carver parsers + signature_engine integration + tests)
- Task 2.3: complete (parallel carve via util/thread_pool.h, scanRange workers, test_carve_parallel)
- Task 2.4: complete (DedupIndex, carver_duplicate source, ResultsView toggle)
- **Tests:** 219/219 native (2 skipped), typecheck + vitest green
- Task 3.1: complete (refs_parser boot/SUPB probe, metadata scan, scan_coordinator + partition probe)
- Task 3.2: complete (exFAT SetChecksum gap tolerance, name offset + stream length fixes, test)
- Task 3.3: complete (APFS dir+extent oid runs attach, omap + oid-tree walk, DirRecWithExtentAttachesRuns test)
- **Tests:** 224/224 native (2 skipped)
- Task 4.1: complete (preview_reader 64KB, NAPI readFilePreview, IPC preload/handlers, ResultsView önizle paneli, preview-utils + vitest)
- Task 4.2: complete (validation.cpp post-recover carve score, RecoveryResult.validationScore, UI Kalite sütunu + rapor Tam/Bozuk)
- Task 4.3: complete (triFragmentedGapCarve 2-gap BGC, signature_engine fallback, test_bgc tri + recovery validation test)
- Task 4.4: complete (password protector 0x2000 stretch KDF, VMK walk, setBitLockerPassword NAPI/UI, openwall test vector, TPM/startup-key error hints)
- **Tests:** 230/230 native (2 skipped), typecheck + vitest green
- Task 5.1: complete (test_recovery_golden.cpp — programmatic FAT/PNG golden; generate-fixtures.ps1; ntfs_deleted README stub)
- Task 5.2: complete (e2e/recovery-flow.spec.ts — results UI smoke; build required for Playwright)
- Task 5.3: complete (scan resume native+UI; CarveScanResumesFromCheckpoint test)
- Task 5.4: complete (bench_scan.cpp WOLF_RUN_BENCH gate; README benchmark table)
- **Tests:** 242/242 native (3 skipped: Ewf, VolumeMapper, BenchScan), typecheck + vitest green
- Task 6.2: complete (refs_integrity CRC32C/CRC64-ECMA, SUPB self-check, entry integrity trailer, recover validation)
- Task 6.1: deferred (GPU PFAC — plan constraint: CPU only)
- Task 6.3: complete (ewf_reader local + http Range byte source; DiskReader attachEwfImage/attachHttpRawImage/attachRawFile)
- Task 6.4: complete (disk_reader_posix.cpp stub — memory/EWF/raw; PhysicalDrive TODO)
- Task 6.5: complete (content_classifier entropy+ext heuristics; signature_engine refine)
- **Tests:** 249/249 native (3 skipped), typecheck + vitest green
