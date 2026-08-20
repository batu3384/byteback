# Byteback

**Professional Windows forensic imaging and data recovery.** Byteback combines a
native C++17 engine with an Electron/React examiner surface. The engine opens
source media with `GENERIC_READ` only; recovered bytes and image output are
written only to user-selected destination paths.

Positioning: **digital forensics** (E01 imaging, hash-chained audit log, USN
timeline, report integrity summary) and **data recovery** (NTFS/FAT/ext4
metadata recovery, signature carving, virtual RAID 0/1/5/6/10, SSD/TRIM
awareness).

## Features

### Native engine (C++17)

- **NTFS** — UTF-16 names, USA fixup, `$STANDARD_INFORMATION` timestamps,
  sparse data runs, LZNT1 decompression, ADS, USN journal parse, INDX slack
  scan, directory tree rebuild. Quick scan: boot `$MFT` LCN run walk. Deep:
  orphan FILE carve.
- **FAT12/16/32 + exFAT** — FAT chain walk (loop-safe), VFAT long names, exFAT
  entry-set state machine, DOS timestamps.
- **Ext2/3/4** — extent tree (multi-level), real names from directory entries,
  deleted inode/dirent evidence.
- **ReFS** — boot/SUPB probe, ministore metadata walk, integrity-stream
  CRC64-ECMA validation (SUPB self-check + resident file trailer).
- **E01 read** — local multi-segment `.E01` and HTTP Range raw images via
  `attachEwfImage` / `attachHttpRawImage` / `attachRawFile` on `DiskReader`.
- **HFS+ / APFS** — HFS+ catalog B-tree (until cancelled). APFS NXSB
  discovery, APSB volume, btree leaf drec (`source=apfs_file`) and file extent
  runs (`apfs_extent`). Catalog: `nx_fs_oid` + first 256 blocks + recursive omap
  btree (not a full container snapshot walk).
- **Carving** — Aho–Corasick signature scan (200+ built-in signatures via embedded
  engine + `resources/signatures-extended.json`),
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

Dashboard, live scan with bad-sector map, directory tree and file detail pane,
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
> `BYTEBACK_EWFINFO` is unset.

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

**Documented limits (no false crypto/product claims):** BitLocker recovery
password supports only the **0x0800 clear-key** protector (TPM/startup-key/password
→ explicit error). FVEK: 64/128 hex or recovery password → AES-128/256-XTS
scan/recover/hex on raw ciphertext images. No GPU PFAC. APFS: `nx_fs_oid` + 256
blocks + **recursive omap btree** (not full snapshot tree). PhysicalDrive wipe:
serial match + typed **IMHA** + OS confirmation; SSD ≠ NIST 800-88. Concurrent
scan/image/wipe operations are serialized.

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

Repository: **https://github.com/batu3384/byteback** (private)

CI workflow (`.github/workflows/build.yml`) is ready locally. Pushing it requires
the `workflow` OAuth scope:

```powershell
gh auth refresh -h github.com -s workflow
git add .github/workflows/build.yml
git commit -m "ci: add GitHub Actions workflow"
git push
```

## License

MIT License
