# Byteback — Architecture

Module map and data flow for contributors. For the examiner-facing feature list,
see [README.md](../README.md).

## Layers and data flow

```
Renderer (React)          Main (Electron)           Native (C++ .node)
----------------          ---------------           ------------------
components/*  --preload-> ipc-handlers.ts --------> bridge_*.cpp
window.api                 native-bridge.ts          |
                                                  +--+----------------+
                                                  | byteback::Engine |
                                                  | ScanCoordinator  |
                                                  | DiskImager/Ewf   |
                                                  | RecoveryEngine   |
                                                  | VirtualRaid      |
                                                  +--------+---------+
                                                           | DiskReader (DeviceIoControl)
                                                  Physical disk (read-only)
```

- The renderer never calls native code directly. `preload/index.ts` exposes a
  allowlisted API via `contextBridge` (`contextIsolation: true`).
- All disk access uses `GENERIC_READ`. The engine does not write to source media
  (image output goes to files, not disks).

## Native modules (`native/src/`)

| Directory | Responsibility | Key files |
|-----------|----------------|-----------|
| `io/` | Raw physical disk I/O, bad-sector telemetry | `disk_reader_win.cpp` |
| `fs/` | Filesystem parsers and layout math | `ntfs_parser.cpp`, `fat_parser.cpp`, `ext4_parser.cpp`, `fat_chain.cpp`, `raid_layout.cpp`, `raid6_math.cpp`, `virtual_raid.cpp`, `partition_scanner.cpp` |
| `carver/` | Signature-based recovery | `signature_engine.cpp`, `bgc.cpp` |
| `recovery/` | Write recovered files to destination | `recovery_engine.cpp`, `preview_reader.cpp`, `validation.cpp` |
| `imager/` | Disk imaging | `disk_imager.cpp`, `ewf_writer.cpp`, `ewf_reader.cpp` |
| `crypto/` | Digests + AES-XTS/CCM + SHA-256 | `md5.cpp`, `aes_xts.cpp`, `aes_ccm.cpp`, `sha256.cpp` |
| `db/` | SQLite metadata store | `metadata_store*.cpp`, `runs_codec.cpp` |
| `forensic/` | Audit log + NSRL MD5 set | `audit_logger.cpp`, `nsrl_lookup.cpp` |
| `smart/` | ATA SMART + NVMe health log | `smart_monitor.cpp` |
| `bridge/` | NAPI bindings by concern | `bridge_{drives,scan,imager,wipe,ops}.cpp`, `napi_bridge.cpp` |
| `security/` | File/free-space wipe; PhysicalDrive serial gate | `data_shredder.cpp` |

## Correctness assurance

Critical math is unit-tested (`native/tests/`; `ctest -C Release` on a typical
dev machine runs 260+ tests, with optional skips):

- **MD5 / SHA-256** — RFC 1321 / RFC 6234 vectors, chain folding tests.
- **GF(2⁸) Reed–Solomon** — field axioms, exponent table, two-disk loss recovery.
- **RAID layout** — stripe→(data, parity) tables and per-stripe disk permutation
  invariants.
- **FAT chain + DOS time** — synthetic tables, loop protection, leap-day vectors.
- **LZNT1 / USA fixup / USN / entropy / EWF container** — round-trip parser tests.

Intentional spec limits (no BitLocker password cracking — FVEK hex enables XTS
decrypt, no GPU PFAC, APFS recursive omap btree not full snapshot, quick NTFS
`$MFT` run walk) are documented in [README.md](../README.md).

## Runtime assets

- `resources/signatures.json` — optional user signature overlay; engine falls
  back to ~114 built-in signatures.
- `<userData>/byteback.db` — scan metadata (WAL).
- `<userData>/byteback.db.audit.log` — SHA-256 hash-chained audit log; reports
  embed recent chain entries.

## Development commands

```bash
npm run build:native   # build C++ engine (cmake-js)
npm run dev            # native + electron-vite dev session
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer/shared)
npm run test:native    # GoogleTest via ctest (count varies)
npm run test:e2e       # Playwright Electron smoke (run npm run build first)
npm run dist           # NSIS installer (release/)
```

Before `test:native`, configure tests:

```bash
cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
```
