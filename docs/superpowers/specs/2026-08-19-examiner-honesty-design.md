# Wolf Recovery — Examiner Honesty & Surface Design

**Date:** 2026-08-19  
**Status:** Draft (audit-driven; implement after plan approval)  
**Spec for:** Phase 7 (post 0–6)  
**Audit:** `docs/codebase-audit/2026-08-19.md`

## Goal

Close the gap between native ops APIs and the examiner: case metadata and NSRL must be usable in the UI; silent limits (HFS 25k, ATA Weibull) must be visible; README must match `ctest`.

## Approaches (chosen)

**A — Examiner honesty (this spec).** Wire existing NAPI. Label limits. Fix docs. No new FS parsers.

**B — FS depth.** APFS omap, `$LogFile` redo, USN↔MFT boost. Deferred: motor already deeper than UI trust.

**C — Scale.** On-disk NSRL, E01 multi-segment >4 GiB. Deferred: no UI for the in-memory set yet.

## In scope

1. Case form (number, investigator, agency, notes) via existing `getCaseInfo`/`setCaseInfo`. Persist SQLite singleton. E01 already reads case in `bridge_imager.cpp`. Report HTML includes the same fields.
2. NSRL: file picker (main `dialog`), `loadNsrl`, stats in UI, optional column/filter on results (`lookupNsrl` per MD5 if hash present — if files have no MD5 yet, show set loaded count only; do not invent hashing of every file this phase unless recover path already hashes).
3. HFS: when catalog hits `kMaxFiles`, emit audit + scan warning payload; UI banner.
4. SMART: ATA `healthScore` panel shows "KALİBRASYONSUZ" copy; NVMe unchanged.
5. Docs: README test count, faz 0–6, `test:native -C Release`, ewf_writer comment, sidebar labels/version.

## Out of scope

- APFS file tree, BitLocker decrypt, GPU PFAC, libFuzzer, multi-case archive, zlib EWF, metadata_store split (note only).

## UX

Dense dark forensic tool (existing Geist/zinc). Case page: form at top, inline errors, `role="alert"` summary on failed save. NSRL load: button + count, not a toast-only failure. No emoji logo. Visible `:focus-visible` on former `outline: none` controls.

## Success

- Examiner sets case number, next E01 header contains it (existing imager path).
- Examiner loads a 2-hash text file, stats show 2.
- HFS fixture or unit test proves cutoff surfaces a warning flag.
- SMART ATA copy contains KALİBRASYONSUZ.
- `npm run typecheck` green; native tests not reduced.
