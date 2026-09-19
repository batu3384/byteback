# Byteback

[![Build and Test](https://github.com/batu3384/byteback/actions/workflows/build.yml/badge.svg)](https://github.com/batu3384/byteback/actions/workflows/build.yml)

**Professional Windows forensic imaging and data recovery.** Byteback combines a
native C++17 engine with an Electron/React examiner surface. The engine opens
source media with `GENERIC_READ` only; recovered bytes and image output are
written only to user-selected destination paths. APFS is image/.E01 only —
there is no Mac app and no Disk Drill Mac parity claim.

Positioning: **digital forensics** (E01 imaging, hash-chained audit log, USN
timeline, report integrity summary) and **data recovery** (NTFS/FAT/ext4
metadata recovery, signature carving, virtual RAID 0/1/5/6/10, SSD/TRIM
awareness). Windows field photo parity is gated on FAT32 USB row B (5/5 MD5),
not on lab fixture counts alone.

## Features

### Native engine (C++17)

- **NTFS** — UTF-16 names, USA fixup, `$STANDARD_INFORMATION` timestamps,
  sparse data runs, LZNT1 decompression, ADS, USN journal parse, INDX slack
  scan, directory tree rebuild. Quick scan: boot `$MFT` LCN run walk (primary
  or last-sector backup boot; `$MFTMirr` rec0 `$DATA` when primary rec0
  is wiped). `$ATTRIBUTE_LIST` pulls unnamed `$DATA`, named ADS, `$EA`, and
  `$REPARSE_POINT` from extension MFT records (resident or non-resident list). `$Bitmap` rec6 follows `$MFT`
  `$DATA` runs (`$MFTMirr` rec0 if primary wiped). Resident or non-resident
  `$EA` emits `name:ea:…` (`ntfs_ea`). `$REPARSE_POINT`
  symlink/mount print name is recoverable (`ntfs_reparse`). EFS
  (`FILE_ATTRIBUTE_ENCRYPTED`) is discovery-only (`ntfs_efs`). Resident
  `$OBJECT_ID` emits Windows GUID as `name:objectid` (`ntfs_object_id`). `$VOLUME_NAME`
  is discovery-only (`ntfs_vol_name`). MFT record view maps
  `mftRef` through the same `$MFT` `$DATA` runs. Deep: orphan FILE carve. Streaming MFT emit:
  2M-record in-suite peak working-set delta stays under 700MB (no full
  `tempFiles` buffer).
- **FAT12/16/32 + exFAT** — FAT chain walk (loop-safe), VFAT long names, exFAT
  entry-set state machine, DOS timestamps. Backup boot: reserved `bkBootSec`
  or sector 6 when primary BPB bytes-per-sector / cluster size is wiped.
  exFAT Backup Boot Region at sector 12 when primary OEM is wiped.
  Volume label dirent (`ATTR_VOLUME` 0x08) is discovery-only (`fat_vol_label`).
  exFAT volume label entry `0x83` is discovery-only (`exfat_vol_label`).
- **Ext2/3/4** — extent tree (multi-level), real names from directory entries,
  deleted inode/dirent evidence. Backup superblock past the 4 MiB spray
  (4 MiB / 8193×1K / 32768×4K). Backup GDT at groups 1/3/5/7 when group-0
  descriptors are wiped. Superblock `s_volume_name` is discovery-only
  (`ext4_vol_name`).
- **XFS** — dir2/dir3 walk, AG1 backup super, unlinked inodes. `sb_fname`
  (12 bytes @108) is discovery-only (`xfs_vol_name`). Inode `di_mtime.t_sec`
  fills `modifiedAt`.
- **ReFS** — boot/SUPB probe, ministore metadata walk, integrity-stream
  CRC64-ECMA validation (SUPB self-check + resident file trailer). Backup
  SUPB at last cluster when cluster 30 magic is wiped.
- **E01 read** — local and HTTP Range multi-segment `.E01`→`.E02` via
  `attachEwfImage` / `attachHttpRawImage` / `attachRawFile` on `DiskReader`.
- **HFS+ / APFS** — HFS+ catalog B-tree (until cancelled), unused leaf
  slots (`hfs_catalog_unused`), committed journal catalog leaves
  (`hfs_journal`; BE/LE `JNLX`, disk never written), TN1150 backup
  volume header at volume-end-1024, resource fork as `name.rsrc`
  (overflow extents forkType 0xFF merged), catalog create/mod dates.
  sibling, and root-folder volume name (`hfs_vol_name`). APFS NXSB (last 4096 backup when primary
  magic is wiped),
  discovery, APSB volume, btree leaf drec (`source=apfs_file`) and file extent
  runs (`apfs_extent`). Inode `j_inode_val` create/mod times fill `createdAt`/`modifiedAt`. Catalog: `nx_fs_oid` + first 256 blocks + recursive omap
  btree (not a full container snapshot walk).
- **ISO 9660 / UDF** — PVD `CD001`, Joliet, Rock Ridge `NM`+`SL` (`iso9660_rr_sl`),
  PVD/Joliet volume identifier (`iso9660_vol_id`),
  El Torito `BOOT.IMG`, multi-extent `byteCount`. Directory recording date
  fills `modifiedAt` (Rock Ridge `TF` MODIFY overrides, `CE` continuation `NM`/`SL`/`TF`). UDF AVDP/VAT/metadata
  partition (mirror + bitmap orphan). LVD volume identifier (`udf_vol_id`)
  and PVD volume identifier (`udf_pvd_id`) and file set identifier
  (`udf_fsd_id`). File Entry Modification Date fills `modifiedAt`; Extended
  File Entry Create Date fills `createdAt`.
- **Carving** — Aho–Corasick signature scan (150 built-in signatures: embedded
  engine + curated `resources/signatures-extended.json` / `-supplement.json`;
  the set is validation-focused — text-magic signatures are intentionally
  excluded because they only generate phantom results),
  structural validation (JPEG/PNG/ZIP/PDF/GZIP/RIFF), clustered BGC recovery
  path for split fragments (sector-stepped, per-scan budget). CPU only; no
  CUDA/OpenCL PFAC.
- **RAID** — 0/1/5/6/10 with GF(2⁸) Reed–Solomon double parity, sector-aligned
  reads. RAID 0 zero-fills bad stripes (scan continues); RAID 1 tries mirror;
  RAID 5/6 uses parity. Members can be marked failed via `fail_disk` (NAPI +
  RAID UI).
- **Imaging** — RAW (dd) and **E01 (EWF)** with inline MD5. E01 supports
  multi-segment chains (`.E01` → `.E02`) with per-segment uint32 tables.
- **SMART** — ATA attributes and NVMe Health Information Log; SSD/TRIM warning.
  ATA health score uses ACS defect counters (realloc 0x05, pending 0xC5).
- **NSRL** — user-selected text/CSV MD5 set (SQLite index). No bundled RDS.
- **Audit** — SHA-256 hash-chained forensic log (RFC 6234 test vectors).

### Examiner UI (Electron + React)

Dashboard, live scan progress bar (bad-sector count, not a layout map), directory tree and file detail pane,
hex viewer (entropy + data templates), RAW/E01 imager (MD5 integrity panel),
SMART panel, virtual RAID builder, USN event timeline, CSV export, HTML/PDF
forensic report with SHA-256 summary, case/NSRL forms, light/dark theme.

## Repository layout

```
byteback/                  # repository root
├── .github/workflows/     # CI
├── docs/                  # ARCHITECTURE, audits, roadmap plans
├── native/                # C++ engine + GoogleTest
├── src/                   # Electron main, preload, React renderer
├── e2e/                   # Playwright smoke tests
├── resources/             # icon, signature JSON
├── package.json
└── README.md
```

## Requirements

- Visual Studio 2022 Build Tools (“Desktop development with C++”)
- Node.js 20+
- CMake 3.20+

## Quick start

```bash
git clone https://github.com/batu3384/byteback.git
cd byteback
powershell -ExecutionPolicy Bypass -File scripts/setup-git-identity.ps1
npm install
npm run dev
```

Run as **Administrator** for `\\.\PhysicalDriveN` access.

## Updates

Byteback does **not** auto-update. This is an examiner tool: silent `electron-updater`
would change the binary under a live case. Install the next version from GitHub
Releases (`npm run dist` locally, or a signed installer when 5.1 code-sign is
available). Crash dumps stay on disk under `%APPDATA%/byteback/CrashDumps`
(`crashReporter` `uploadToServer: false`) — no telemetry.

## Commands

```bash
npm run build:native   # build C++ engine (cmake-js)
npm run build          # native + electron-vite production build
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer/shared)
npm run test:native    # GoogleTest via ctest -C Release
npm run test:e2e       # Playwright Electron smoke (requires npm run build)
npm run dist           # NSIS x64 installer (release/)
```

> `build:native` resets the test generator cache. Before `test:native`, configure
> with `cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON`. Test count
> follows `ctest -C Release` output; `Ewf.OptionalEwfinfoCrossCheck` skips when
> `BYTEBACK_EWFINFO` is unset. Local gate 2026-09-15: ctest 641 (4 skipped),
> vitest 390, playwright 38.

## Benchmarks (developers)

| Scenario | Environment | Notes |
|----------|-------------|-------|
| 64 MiB memory volume, deep scan | `BYTEBACK_RUN_BENCH=1 ctest -R BenchScan` | Synthetic disk; not real SSD/HDD |
| 500 GB sparse deep scan | Planned | Not automated in CI yet |

Results are machine-dependent; reference only, not marketing claims.

## Roadmap status

1. **Phases 0–6 (engine)** — reliability, NTFS depth, VSS, RAID/batch, content
   FTS, Apple FS, ops (case SQLite + NSRL NAPI).
2. **Phase 7 (examiner surface)** — Case/NSRL page, ATA ACS labels, README/ctest
   alignment.
3. **Audits** — dated reports under `docs/codebase-audit/` are living documents.
   A “all fixed” claim in one run does not close the next audit.

**Documented limits (no false crypto/product claims):** BitLocker unlock via
**FVEK** (64/128 hex), **0x0800 recovery password**, or **0x2000 user password**
(Windows only; TPM-only / startup-key protectors → explicit error). Scan/recover/hex
on decrypted ciphertext when FVEK is set. No GPU PFAC. APFS: `nx_fs_oid` + 256
blocks + **recursive omap btree** (not full snapshot tree). VSS: shadow-copy
metadata enumeration during quick scan (not full VSS mount workflow). PhysicalDrive
wipe: serial match + typed **IMHA** + OS confirmation; SSD ≠ NIST 800-88. Concurrent
scan/image/wipe/content-search serialized via `heavyOps`.

## Security

- Administrator elevation required for sector access (NSIS manifest
  `requireAdministrator`). Renderer uses `sandbox` + `contextIsolation`; native
  addon loads only in the main process.
- Engine opens source media with `GENERIC_READ`. `FILE_SHARE_READ` first; falls
  back to `FILE_SHARE_WRITE` when the volume is locked — not a write grant; host
  OS may still modify evidence without hardware write blockers.
- Recovery and imaging write only to user-chosen destinations. Recover rejects
  renderer-supplied `runs`; SQLite `fileId` + `scanId` are required.
- Full-disk PhysicalDrive wipe: Shredder UI + serial confirmation + OS dialog.
  Native layer refuses without serial match. File/free-space wipe still rejects
  device paths (CA-001). SSD wipe is not NIST 800-88 sanitization.
- NSRL path is chosen via main-process file dialog, not from renderer input.

## GitHub

Repository: **https://github.com/batu3384/byteback**

CI: GitHub Actions [`build.yml`](.github/workflows/build.yml) on every push to
`main` — typecheck, native build + ctest, Vitest, Playwright, unpackaged dist smoke.

## License

MIT License
