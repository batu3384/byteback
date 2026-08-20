# Architecture

## Layers

```
Renderer (React)     Main (Electron)        Native (byteback_engine.node)
----------------     ---------------        ------------------------------
components/*  ──►    ipc-handlers.ts  ──►   bridge_*.cpp
window.api           native-bridge.ts         ScanCoordinator, RecoveryEngine, …
                                              DiskReader (DeviceIoControl, READ)
```

- Renderer never loads the native addon. `preload/index.ts` exposes an allowlisted API (`contextIsolation: true`, `sandbox: true`).
- Source media: `GENERIC_READ` only. Writes go to user-selected files (recovery, imaging), not to evidence disks.

## Native modules (`native/src/`)

| Module | Role |
|--------|------|
| `io/` | Physical disk I/O, bad-sector telemetry |
| `fs/` | NTFS, FAT/exFAT, ext4, ReFS, APFS/HFS parsers, RAID layout |
| `carver/` | Aho–Corasick signatures, BGC gap carving |
| `recovery/` | Sparse/LZNT1 extract, validation, preview |
| `imager/` | RAW + E01 write/read |
| `db/` | SQLite scan metadata |
| `forensic/` | Hash-chained audit log, NSRL MD5 index |
| `bridge/` | NAPI entry points split by concern |
| `security/` | Wipe with device-path guards |

## Metadata flow

1. `ScanCoordinator` drives quick/deep/full_carve pipelines.
2. Found files → `MetadataStore` (SQLite) with stable row IDs.
3. Recover/preview IPC requires `scanId` + `fileId` — loads `runs` from DB, not renderer JSON.

## Tests

| Suite | Command |
|-------|---------|
| Native | `ctest -C Release` in `native/build` |
| TS unit | `npm run test` (Vitest) |
| E2E | `npm run test:e2e` (Playwright + built `out/`) |

Full module map: `docs/ARCHITECTURE.md` in the repository.
