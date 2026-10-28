# Audit notes (working)

Date: 2026-10-28. Read-only snapshot from the original pass. **Current remediation status:** `audit/REMEDIATION.md`, honest v1 gaps: `audit/HONEST_PRODUCT_GAPS.md`.

## Pass 1 — Inventory and threat model

- Product: Byteback 0.1.0. Electron 44 + React 18 examiner UI + C++ N-API engine (`byteback_engine.node`). Windows NSIS, `requestedExecutionLevel: requireAdministrator` (`package.json`).
- User: local examiner/operator with admin rights. Not a multi-tenant web service. No login, no remote API.
- Critical assets: evidence disks and images, recovered files, SQLite case DB (`userData/byteback.db`), hash-chained audit log, BitLocker/LUKS key material in process memory, wipe targets.
- Trust boundary: renderer is sandboxed (`contextIsolation`, `nodeIntegration: false`, `sandbox: true` in `src/main/main.ts`). Preload allowlists IPC. Native runs in the main process and opens `\\.\PhysicalDriveN` read-only except explicit wipe.
- Worst case: crafted evidence image or regex hits a native memory/CPU bug inside the admin process; or a wipe confirmation is satisfied and the wrong disk is overwritten. Court use is not claimed (`docs/release-checklist.md` still open).

## Pass 2 — Evidence

**Snapshot at first audit pass.** Items marked fixed in `audit/REMEDIATION.md` / `OBJECTIVE_CHECKLIST.md` override the bullets below (e.g. ReDoS filename guard, WAL `synchronous=FULL`, POSIX TU gate, ErrorBoundary, dev audit).

- `tsc --noEmit -p tsconfig.node.json`: exit 0 (2026-10-28).
- `npm audit --omit=dev`: 0 vulnerabilities.
- `npm audit` (includes dev): **remediated** — vitest 4.1.12 + electron-vite 6 stack; CI `npm audit` → 0 (was vitest/xmldom advisories at pass time).
- `git log -S AKIA` / `BEGIN PRIVATE`: no hits in last scanned range. `fvek` hits are feature commits, not embedded keys.
- HTTP image: engine rejects `isHttpUrl`; picker/IPC local paths only — README aligned (was misleading E01 HTTP sentence).
- `httpUrlHostAllowed` blocks literal private IPv4 and localhost, then blocks any host whose lowercased name starts with `fc` or `fd` (`native/src/io/byte_source.cpp`). No DNS resolution check. Not reachable from the file-picker path today.
- Filename regex — **remediated:** `contentRegexQueryOk` on filename search (native + IPC + tests).
- Case DB WAL — **remediated:** `synchronous=FULL` + `WalSyncFullVsNormalBulkInsertEvaluation`.
- POSIX CI — **remediated:** `native/scripts/posix-syntax-check.sh` (`find`-derived TU set).
- ErrorBoundary — **remediated:** `AppErrorBoundary` + tests.
- Recover dest: main `validateRecoverDestDir` before native. Wipe requires serial match plus dialog plus phrase `IMHA`.
- SQLite amalgamation `3.53.4` (2026-07-24) in `native/third_party/sqlite3.h`.
- Renderer report HTML escapes case fields and audit lines. Hash interpolated into the PDF HTML is a local SHA-256 hex.

## Pass 3 — External sources (2026-10-28)

- Disk Drill 6 Windows vs Mac matrix: https://www.cleverfiles.com/help/disk-drill-mac-windows.html
- R-Studio product page: https://www.r-studio.com/
- R-Studio Technician: https://www.r-studio.com/Data_Recovery_Technician.shtml and https://www.r-studio.com/Unformat_Help/r-studio-technician.html
- NIST SP 800-88 Rev. 2 (September 2025): https://csrc.nist.gov/pubs/sp/800/88/r2/final and https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-88r2.pdf
- CleverFiles comparison table (page dated 2025): https://www.cleverfiles.com/howto/rstudio-alternative.html

## Not run (original pass snapshot)

Superseded for roadmap gates by **`npm run verify:audit`** (490 Vitest + typecheck + npm audit) and Windows **`npm run verify:audit:full`** (native audit ctest subset + `posix-syntax-check.verify`). Full Release `ctest`, Playwright e2e, eslint (no project eslint script), semgrep, gitleaks, trivy, osv-scanner were not part of that first read-only pass.
