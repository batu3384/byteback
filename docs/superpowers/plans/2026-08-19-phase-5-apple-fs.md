# Phase 5 — Apple FS Depth

## Delivered

- [x] HFS+ extent overflow btree — merges 9+ fork extents via `extentsFile`
- [x] HFS+ volume header fork offsets corrected (catalog @272, extents @192)
- [x] APFS container walk — NXSB parse + APSB volume enumeration (`apfs_container`, `apfs_volume`)
- [x] Tests: `test_apple_fs.cpp` (+2)

## Conscious limits

- APFS: volume superblock discovery only (no omap/file tree)
- HFS: overflow lookup when inline fork is full and logical size exceeds mapped bytes

## Next

Phase 6 (Ops) — see `2026-08-19-phase-6-ops.md`.
