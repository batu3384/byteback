# Audit objective checklist

Maps the thread goal to evidence. **Phase 4 operator items cannot be marked done in repo alone.**

| Requirement | Status | Evidence |
|-------------|--------|----------|
| Filename ReDoS parity with content search | **Done** | Native `contentRegexQueryOk`; IPC `content-regex-query.ts`; `MetadataStoreTest.SearchFilesRegexReDoSPatternRejected`; `ipc-handlers-scan-db.test.ts` |
| README vs evidence I/O | **Done** | `README.md`; `isEvidenceImagePath` rejects http (test); `dash.imageScanHint` |
| POSIX syntax TU list (CI) | **Done** | `native/scripts/posix-syntax-check.sh`; CI job |
| React ErrorBoundary | **Done** | `AppErrorBoundary` + scan id / reload; `log-renderer-error`; Vitest |
| SQLite WAL + synchronous | **Done** | `PRAGMA synchronous=FULL`; `CaseDbUsesWalFullSync`; `WalSyncFullVsNormalBulkInsertEvaluation` |
| IPC / `vhd_source` split (audit scope) | **Done** | IPC modules; `vhdx_source.cpp`; `kVhdFamilyMaxExpandBytes` in `vhd_source_internal.h` + test |
| Dev audit advisories | **Done** | `npm audit` 0; CI gates; electron-vite 6 / vitest 4 |
| Honest product gaps | **Done** | `audit/HONEST_PRODUCT_GAPS.md`, `release-checklist.md` |
| Field USB A/B/C | **Open** | Runbook §1; `FIELD_USB_LOG.md`; `[x]` without log PASS → `phase4:status` DISHONEST |
| Authenticode release sign | **Open** | Runbook §2; `PHASE4_SIGN_LOG.md`; `[x]` without log PASS → DISHONEST |
| NSIS clean-box smoke | **Open** | Runbook §3; `PHASE4_VM_SMOKE_LOG.md`; `[x]` without VM log PASS → DISHONEST |
| Tagged CHANGELOG / v1.0 | **Open** | Runbook §4; released `## [x.y.z]` in CHANGELOG; `[x]` without section → DISHONEST |

Gate command (engineering): `npm run verify:audit` (Windows: `npm run verify:audit:full`). **Phase 4:** `npm run phase4:status`. **Full thread goal probe:** `npm run verify:goal` (exit 0 only when engineering + Phase 4 checklist closed).
