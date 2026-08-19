# Phase 3 — RAID Scan & Batch Recovery

## Delivered

- [x] `tagRaidScanSource()` — scan results prefixed `raid_*` when VirtualRaid backend active
- [x] RAID scan via `startScan(-1)` → drive path `raid`
- [x] `getRaidState` NAPI + IPC
- [x] `recoverFilesBatch` — native batch recovery + IPC
- [x] RaidBuilder auto-starts quick scan after assembly
- [x] ResultsView uses batch API for multi-select; RAID recovery without physical drive index
- [x] Tests: `test_raid_scan.cpp`

## Next (Phase 4)

- Async content search + content FTS
- DB-side category filters
