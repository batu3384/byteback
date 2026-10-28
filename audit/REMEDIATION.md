# Audit roadmap remediation (2026-10-28)

Tracks `AUDIT_REPORT.md` §9 phases against the working tree. **No item marked done without a test or CI step.** Engineering gate: `npm run verify:audit`. Full goal map: `audit/OBJECTIVE_CHECKLIST.md`.

## Phase 1 — DONE

| Item | Evidence |
|------|----------|
| Filename regex ReDoS guard | Native `contentRegexQueryOk`; IPC `search-files` / content search handlers; `content-regex-query.ts`; `MetadataStoreTest.SearchFilesRegexReDoSPatternRejected` |
| README vs HTTP E01 | `README.md` — local chains only; engine HTTP not in IPC |
| No false 1.0 tag | Still `0.1.0`; `release-checklist.md` |

## Phase 2 — DONE

| Item | Evidence |
|------|----------|
| POSIX syntax CI list | `native/scripts/posix-syntax-check.sh` + `posix-syntax-check.verify.sh`; CI job |
| Case DB WAL + FULL sync | `PRAGMA synchronous=FULL`; `CaseDbUsesWalFullSync`; `WalSyncFullVsNormalBulkInsertEvaluation` (ratio in test properties; `BYTEBACK_WAL_EVAL_ROWS` for 2M scale) |
| Production npm audit | `npm run audit:prod` → 0; CI `verify:audit` job |
| Dev npm audit | `electron-vite@6`, `vite@8`, `vitest@4.1.12`, `electron-builder@26.18.0`; CI `npm audit` → 0 |

## Phase 3 — DONE (scoped split, not full vhd monolith removal)

| Item | Evidence |
|------|----------|
| React ErrorBoundary | `AppErrorBoundary.tsx` (scan-live reload vs scan id); `log-renderer-error` → session log; Vitest |
| IPC modularization | `ipc-handlers-scan-db.ts`, `ipc-handlers-recover.ts`, `ipc-utils.ts`, `ipc-db-state.ts`, …; `ipc-handlers.ts` still ~1136 lines (orchestration) |
| `vhd_source` split | `vhdx_source.cpp` + `vhd_source_internal.h` (`kVhdFamilyMaxExpandBytes`); `vhd_source.cpp` still large (VMDK/QCOW2/VDI/DMG) |
| VHD extract fuzz | `native/tests/test_fuzz_vhd_extract.cpp` |
| Wipe serial re-read | `bridge_wipe.cpp` + IPC post-dialog refresh (`ipc-handlers.ts`) |

## Phase 4 — OPEN (cannot fake PASS)

| Item | Status |
|------|--------|
| Field USB A/B/C | Checklist `[ ]` — `FIELD_USB_LOG.md`; `phase4:status` DISHONEST if `[x]` without log PASS |
| Authenticode | Checklist `[ ]` — `PHASE4_SIGN_LOG.md` + `CSC_*`; DISHONEST if `[x]` without log PASS |
| NSIS clean-box smoke | Checklist `[ ]` — `PHASE4_VM_SMOKE_LOG.md`; lab exe via `release:dist:unsigned` (2026-10-28) ≠ VM PASS |
| Tagged CHANGELOG / tag | Checklist `[ ]` — released `## [x.y.z]` required; DISHONEST if `[x]` while only Unreleased |
| Closure probe | `npm run verify:goal` = `verify:audit:full` + `phase4:status` (exit 0 only when all Phase 4 rows honestly closed) |
| Streaming virtual disk / §8 product gaps | Documented in `audit/HONEST_PRODUCT_GAPS.md` — not v1 claims |
| Lab Playwright | 45/45 local 2026-10-28; CI Windows job |

## Verification run this session

- `npm run verify:audit:full` → engineering PASS (490 Vitest incl. `audit-phase4-gate.test.ts` 120s budget on Windows; native subset; Git Bash or skip broken WSL `bash`; release-dist self-check)
- `npm run phase4:status` → exit 1 (6 checklist rows OPEN — expected until operator closes Phase 4)
