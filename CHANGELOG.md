# Changelog

All notable changes to this project are documented here. Version `0.1.x` is pre-1.0;
see [docs/release-checklist.md](docs/release-checklist.md) before tagging `v1.0.0`.

## [Unreleased]

### Added (parity round, 2026-11-26)

- Btrfs parser: superblock with backup fallback, chunk-map logical→physical
  translation, subvolume/root/fs-tree walk, real INODE_REF paths, extent runs,
  deleted recovery from older-generation backup roots (`btrfs_stale_root`).
  Multi-device RAID5/6 metadata is refused honestly instead of mistranslated.
- Carve signature table expanded to ~230 formats: executables (ELF, Mach-O,
  DEX, .pyc, BEAM), archives (WIM, LHA, cpio, LZFSE), embedded images
  (SquashFS, UBIFS, Android sparse/boot, CHD, WBFS, RVZ), media (MXF, R3D,
  DPX, CAF, RF64, DTS) and forensic artifacts (hibernation, minidump/full
  dump, NTFS INDX/RCRD/RSTR, index.dat, bplist, keychain, fsevents, LUKS,
  BitLocker, Android backup).
- RFC 3394 AES Key Wrap/unwrap — FileVault2 KEK prerequisite.

### Fixed (remediation rounds K1–K7 / Y1–Y9, 2026-11-23..26)

- APFS: real B-tree parse (kvloc/kvoff table-of-contents, hashed dir keys,
  omap oid→paddr deferred resolution, checkpoint/stale scan, parent-oid path
  reconstruction) — previously no file was recoverable from a real APFS disk.
- ext4: correct jbd2 tag layout, 256 MiB journal budget, all-group inode table
  walk, deleted dirent recovery from directory slack, real paths.
- NTFS: LZNT1 decompression per 64 KiB compression unit; overwritten-cluster
  detection rescales confidence via live/deleted run overlap.
- FAT12/16/32: real directory paths; deleted-file chains stop at live-owned
  clusters instead of gluing foreign data.
- Imaging: ddrescue-style skip-ahead after 8 consecutive dead chunks, trim
  pass retries merged failed-run edges and patches `.part` in place with a
  re-hash; granular bad-sector bisect in recovery reads.
- Lost-partition scan: filesystem sizes parsed from boot/superblock fields,
  1 MiB stepping with 63-sector fallback, HFS+/APFS/XFS/ReFS/Btrfs/LDM/LVM
  signatures; unread sectors no longer scan as empty.
- Honesty: all-zero (TRIMmed/overwritten) recovery reports failure with an
  explanation; ReFS is discovery-only rather than fabricated content; carving
  drops text-magic signatures that produced phantom files.
- e2e: spawned Electron strips inherited `ELECTRON_RUN_AS_NODE` (CI bug that
  failed all 45 specs as "process failed to launch").


### Fixed (audit 2026-11-21)

- Audit log: a relaunch continues the hash chain from the last link (it restarted from zeros, so every second session verified as broken).
- Non-ASCII (UTF-8) paths: CSV export, NSRL list, signature JSON and the audit log open under folders such as `Masaüstü`.
- Carve stop/resume: a stop mid-carve stays resumable, the resume point never skips unscanned or half-read files, parallel carve progress is no longer over-counted.
- Database: `files_fts` triggers made every `files` DELETE fail; repaired on open. A scan resumed in its metadata phase no longer lists every file twice.
- Imaging: one completion tick after the image is final; a stopped run reports cancelled; unreadable sectors are shown; the fake latency chart is gone.
- RAID: RAID 5 right-symmetric and the four Linux md RAID 6 layouts follow `drivers/md/raid5.c`; RAID carve rows stay recoverable.
- Safety: a PhysicalDrive holding Windows or the Byteback data folder cannot be wiped.
- UI: image-scan results can be previewed/recovered; report uses local date and "allocated" excludes carved rows; hex view shows full 4Kn sectors; RAID scans start on request (quick or deep).

### Fixed (adversarial review, 2026-11-22)

- RAID 6 detection without metadata now scores all four mdadm layouts; parity
  ties prefer the symmetric (mdadm-default) variant and FS signatures still
  refine the choice.
- Parallel carve: a dead worker no longer stores a "complete" position —
  progress and the resume cursor stop where work actually finished.
- Wipe guard resolves the Windows system volume via `GetSystemDirectoryW`
  (new `getSystemDriveLetter` binding), not a spoofable env var.
- Imaging: the cancel signal is an explicit native `stopped` flag; a finished
  image is no longer inferred from a missing MD5.
- `clearScanRecords` only tolerates a missing content-index table at prepare;
  a failing delete is a real error. Audit reopen accepts a chain hash only at
  end of line. Carve-family source prefix now has a single definition.
  Paused quick scans explain why resume is disabled.

### Security & reliability (audit 2026-10-28)

- Filename search: `contentRegexQueryOk` guard in native bridge and metadata store (ReDoS parity with content search); IPC pre-check via `src/shared/content-regex-query.ts`.
- Case DB: SQLite WAL with `PRAGMA synchronous=FULL`; `WalSyncFullVsNormalBulkInsertEvaluation` measures FULL vs NORMAL bulk insert cost (default 25k rows; `BYTEBACK_WAL_EVAL_ROWS` for 2M scale).
- Physical wipe: re-read drive serial immediately before write (IPC post-dialog + native worker).
- Renderer: root `AppErrorBoundary` with tests.

### Architecture & CI

- IPC split: scan DB/search, recover/preview, shared utils, evidence context, audit bridge.
- Native: `vhdx_source.cpp` extracted from `vhd_source.cpp`; POSIX syntax CI uses `find`-derived TU set (not hand list).
- Shared VHD expand cap: `kVhdFamilyMaxExpandBytes` in `vhd_source_internal.h` (64 MiB; not streaming).
- Fuzz: `test_fuzz_vhd_extract.cpp` (mutated VHD/VHDX headers must not throw).
- README: local E01 chains only; HTTP Range not wired to file picker or IPC.
- Supply chain: electron-vite 6 / vite 8; vitest 4.1.12; CI runs full `npm audit`.
- Phase 4 gates (operator): `npm run phase4:status`, `npm run verify:goal`; log/checklist parity checks (no fake `[x]`).

### Still open (not claimed fixed)

- Field USB rows A/B/C, Authenticode signing, NSIS clean-box smoke, streaming virtual-disk read.
