# Changelog

All notable changes to this project are documented here. Version `0.1.x` is pre-1.0;
see [docs/release-checklist.md](docs/release-checklist.md) before tagging `v1.0.0`.

## [Unreleased]

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
