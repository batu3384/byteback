# Phase 7 â€” Examiner Honesty Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make case/NSRL reachable from the renderer, surface HFS and SMART limits honestly, and align README/test scripts with `ctest`.

**Architecture:** Existing NAPI in `bridge_ops.cpp` stays. Preload whitelist + one CaseOps view. HFS cutoff returns a coordinator warning bit. No new native deps.

**Tech Stack:** C++17, NAPI v8, Electron IPC, React, GTest, Vitest.

**Spec:** `docs/superpowers/specs/2026-08-19-examiner-honesty-design.md`

## Global Constraints

- C++17, CMake 3.20+, no new runtime deps
- `ctest -C Release` green; `npm run typecheck` green
- NSRL path only from main-process file dialog, not renderer string
- Case remains SQLite singleton (`case_info` id=1)

## Files

- Modify: `src/preload/index.ts`, `src/shared/types.ts`, `src/shared/ipc-contract.ts`, `src/main/ipc-handlers.ts`
- Create: `src/renderer/components/CaseView/CaseView.tsx` (+ css)
- Modify: `App.tsx`, `Sidebar.tsx`, `ReportGenerator.tsx`, `SmartView.tsx`
- Modify: `hfs_catalog.cpp` / `hfs_catalog.h`, `scan_coordinator.cpp` (warning hook)
- Modify: `package.json`, `README.md`, `ewf_writer.h`
- Modify: focus CSS files listed in CA-012
- Test: `test_hfs` warning or extend `test_case_info.cpp`; renderer not required for native flags

---

### Task 1: Preload + IPC dialog for case/NSRL

**Files:**
- Modify: `src/preload/index.ts`
- Modify: `src/shared/types.ts`
- Modify: `src/shared/ipc-contract.ts`
- Modify: `src/main/ipc-handlers.ts`

**Interfaces:**
- Consumes: existing `get-case-info`, `set-case-info`, `load-nsrl`, `lookup-nsrl`, `get-nsrl-stats`
- Produces: `window.api.getCaseInfo()`, `setCaseInfo(info)`, `pickNsrlFile()`, `loadNsrl(path)` only after pick (main may combine pick+load)

- [ ] **Step 1: Add types**

```ts
export interface CaseInfo {
  caseNumber: string
  investigator: string
  agency: string
  notes: string
  createdAt: number
  updatedAt: number
}

export interface NsrlStats {
  ok?: boolean
  count: number
  path: string
}
```

- [ ] **Step 2: Preload whitelist**

```ts
getCaseInfo: () => ipcRenderer.invoke('get-case-info'),
setCaseInfo: (info: Partial<CaseInfo>) => ipcRenderer.invoke('set-case-info', info),
pickAndLoadNsrl: () => ipcRenderer.invoke('pick-and-load-nsrl'),
getNsrlStats: () => ipcRenderer.invoke('get-nsrl-stats'),
lookupNsrl: (md5: string) => ipcRenderer.invoke('lookup-nsrl', md5),
```

- [ ] **Step 3: Main `pick-and-load-nsrl`**

Use `dialog.showOpenDialog` filters `txt,csv`. Then `engine.loadNsrl(filePaths[0])`. Do not add a preload method that takes a raw filesystem path.

- [ ] **Step 4: Typecheck**

Run: `cd byteback && npm run typecheck`  
Expected: PASS

- [ ] **Step 5: Commit** (only if the user asked to commit this task)

---

### Task 2: CaseView UI + report fields

**Files:**
- Create: `src/renderer/components/CaseView/CaseView.tsx`
- Create: `src/renderer/components/CaseView/CaseView.css`
- Modify: `App.tsx`, `Sidebar.tsx` (`id: 'case'`, label `Dava`)
- Modify: `ReportGenerator.tsx` â€” load `getCaseInfo`, put case number/investigator in HTML header

**Interfaces:**
- Consumes: Task 1 `window.api`
- Produces: saved CaseInfo visible after reload; report HTML contains `caseNumber`

- [ ] **Step 1: Form**

Fields: caseNumber, investigator, agency, notes. Save button. On failure, `role="alert"` summary + per-field text. Do not toast-only.

- [ ] **Step 2: Wire App page `'case'`**

- [ ] **Step 3: Report**

`getCaseInfo` before building HTML; if empty, show "Dava numarasÄ± yok" â€” do not invent a number.

- [ ] **Step 4: Typecheck**

Run: `npm run typecheck`  
Expected: PASS

---

### Task 3: HFS 25k warning (not silent)

**Files:**
- Modify: `native/include/fs/hfs_catalog.h`
- Modify: `native/src/fs/hfs_catalog.cpp`
- Modify: `native/src/scan_coordinator.cpp` (or FileRecord marker)
- Modify: `native/tests/test_apple_fs.cpp`

**Interfaces:**
- Consumes: `CatalogCtx::kMaxFiles`
- Produces: when cutoff hits, insert a `FileRecord` with `name="[HFS] catalog truncated at 25000"` and `source="hfs_limit"` OR set a scan-level flag persisted in `scans` â€” prefer one extra FileRecord so UI/results already show it without schema change.

- [ ] **Step 1: Failing test**

```cpp
TEST(HfsCatalog, EmitsSentinelWhenHittingMaxFiles) {
  // After walk with kMaxFiles forced to 1 in test build OR count sentinel
  // Assert at least one record source == "hfs_limit" when catalog has 2+ files
}
```

If changing `kMaxFiles` globally is too blunt, add `scanHfsPlusCatalog(..., int maxFiles = 25000)` test-only overload.

- [ ] **Step 2: Run test, expect FAIL**

Run: `ctest -C Release -R HfsCatalog.EmitsSentinel --output-on-failure`

- [ ] **Step 3: Implement sentinel + `AuditLogger` event `HFS_CATALOG_TRUNCATED | count=`**

- [ ] **Step 4: Test PASS**

---

### Task 4: SMART KALÄ°BRASYONSUZ copy

**Files:**
- Modify: `src/renderer/components/SmartView/SmartView.tsx`

- [ ] **Step 1:** Under ATA (non-NVMe) score, add paragraph: `ATA saÄŸlÄ±k skoru KALÄ°BRASYONSUZ heuristic (Î·=500, Î²=1.5). Ã–mÃ¼r tahmini deÄŸil.` NVMe block unchanged.

- [ ] **Step 2:** Typecheck PASS

---

### Task 5: Docs + test script + sidebar honesty

**Files:**
- Modify: `README.md` (test count 145, Faz 0â€“6, NSRL: "UI Phase 7" or after Task 1â€“2 "kullanÄ±cÄ± dosyasÄ±")
- Modify: `package.json` `test:native` add `-C Release`
- Modify: `Sidebar.tsx` label `Ä°maj (RAW / E01)`; version from a constant matching `0.1.0`; replace emoji with `resources/icon.svg` or lucide `Shield`
- Modify: `ewf_writer.h` CA-004 comment: optional `ewfinfo` / skip
- Modify: `ImagerView.css`, `HexEditor.css`, `ShredderView.css`, `RaidBuilder.css`, `KeywordSearch.css` â€” `outline: none` â†’ `:focus-visible { outline: 2px solid currentColor; outline-offset: 2px; }`

- [ ] **Step 1:** Apply copy/CSS
- [ ] **Step 2:** `npm run typecheck`
- [ ] **Step 3:** Native `ctest -C Release` still 145 (or +1 from Task 3)

---

## Spec coverage

| Spec item | Task |
|-----------|------|
| Case form + E01 existing path | 1â€“2 |
| NSRL picker | 1 (+ optional stats on CaseView) |
| HFS warning | 3 |
| SMART label | 4 |
| README / script / a11y | 5 |
| metadata_store split | out of scope (CA-005 note) |
| APFS omap / E01 4 GiB | out of scope |

## Placeholder scan

None: no TBD, no "add validation later".
