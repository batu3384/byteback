# Phase 0 Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship trust-layer fixes: E2E scanâ†’recover proof, honest dashboard state, MPEG-TS FOV, VSS creation timestamps.

**Architecture:** Extend existing `file_validators.h` dispatch, add one integration test compiling scan+db+recovery sources, minimal UI guard on scan status enum.

**Tech Stack:** C++17, GTest, SQLite metadata store, Electron/React dashboard.

**Spec:** `docs/superpowers/specs/2026-08-19-byteback-professional-roadmap-design.md`

## Global Constraints

- C++17, CMake 3.20+, no new dependencies
- `ctest -C Release` green before done
- Scan DB status: `0=Running`, complete stored as `1` via bridge (legacy mapping)

---

### Task 1: MPEG-TS structural validator

**Files:**
- Modify: `native/include/carver/file_validators.h`
- Modify: `native/src/carver/signature_engine.cpp` (`dispatchValidator`)
- Modify: `native/tests/test_validators.cpp`

**Interfaces:**
- Produces: `int validateMpegTs(const uint8_t* data, size_t size)` â€” 0 reject, â‰¥90 accept

- [x] Add `validateMpegTs` â€” require 5Ã—188-byte packets each starting with `0x47`
- [x] Wire `ext == "ts"` in `dispatchValidator`
- [x] Tests: valid TS buffer scores high; `0x47 0x40 0x00` junk scores 0

### Task 2: E2E scan â†’ recover test

**Files:**
- Create: `native/tests/test_e2e_scan_recover.cpp`
- Modify: `native/CMakeLists.txt`

- [x] FAT16 MBR fixture quick scan inserts into temp MetadataStore
- [x] Recover `TEST.TXT`, assert payload `Hello FAT16` and MD5 present

### Task 3: Dashboard active session fix

**Files:**
- Modify: `src/renderer/components/Dashboard/Dashboard.tsx`

- [x] Resume banner + stats only when `getScanState().status === 0`

### Task 4: VSS creation time

**Files:**
- Modify: `native/include/fs/vss_scanner.h`
- Modify: `native/src/fs/vss_scanner.cpp`
- Modify: `native/src/scan_coordinator.cpp`

- [x] Add `createdAt` field; populate via `GetFileTime` on shadow copy handle
- [x] Emit `fr.createdAt = snap.createdAt` for VSS records
