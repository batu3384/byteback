# Adversarial review — Byteback

- Date: 2026-08-19
- Scope: `` native + Electron (full product, HEAD)
- Method: three mandatory personas (Saboteur, New Hire, Security Auditor); 2+ personas = one level promotion
- Runtime: static code read + prior `ctest` 146/146 (unit tests do not cover production teardown / 4 GiB E01 / real FVE OEM)
- Verdict: **BLOCK**

Previous audit (`2026-08-19.md`) closed Case/NSRL surface, HFS sentinel, SMART label, README alignment. This run exposes evidence integrity, privilege IPC, and native crash/OOB paths *under* those patches.

## Summary counts

| Level | Count |
|--------|------|
| CRITICAL | 12 |
| WARNING | 14 |
| NOTE | 8 |

## Persona notes

- **Saboteur:** production scan 2 → `std::terminate`; HFS hostile volume OOB; recover zero + `success=true`; E01 wrap; shared `DiskReader`.
- **New Hire:** `status` has three meanings (`byteback_db.h` Intact vs NTFS IN_USE vs report "overwritten"); RAID 6 `fail_disk` not in NAPI; CoC paragraph contradicts engine.
- **Security Auditor:** `startWipe` still renderer string; recover `FileRecord.runs` from renderer; `requireAdministrator` + `sandbox: false` + `asar: false`; report HTML XSS.

---

## Critical Findings

### AR-001 | Recover trusts renderer `runs` — not DB

- Persona: Security Auditor, Saboteur → **CRITICAL**
- Path: `native/src/bridge/bridge_wipe.cpp` `FileRecordFromJs` / `RecoverFile`; `byteback_db.h` `getFileById` (unused)
- Evidence: Recover reads via IPC object `runs[].startSector/sectorCount`. `scanId` only for `incrementRecovered`.
- Harm: Admin process writes arbitrary sector range as "recovered + MD5".

### AR-002 | `startWipe` raw file path, no confirmation

- Persona: Security Auditor → **CRITICAL**
- Path: `src/main/ipc-handlers.ts` `start-wipe`; `preload/index.ts`; `bridge_wipe.cpp` `StartWipe`
- Evidence: Shredder UI locks disk wipe. Preload exposes `startWipe(targetPath)`. Native rejects only `PhysicalDrive` / `\\.\`.
- Harm: Renderer (XSS/DevTools) destroys case file in 3 passes.

### AR-003 | E01 >4 GiB native still writes, table `uint32` wrap

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Path: `ewf_writer.cpp` `write` `static_cast<uint32_t>(currentChunkBytes_)`; `finish()` true
- Evidence: UI confirm exists; writer does not reject. Test only small synthetic.
- Harm: Court E01 broken offset table + "valid" MD5.

### AR-004 | Bad sector → zero, `success=true` / image complete

- Persona: Saboteur, Security Auditor, New Hire → **CRITICAL** (promoted)
- Path: `recovery_engine.cpp` ~157–183; `disk_imager.cpp` ~84–96
- Evidence: Read fail writes zeros, MD5 includes zeros, result success.
- Harm: Bit-for-bit / recovered claim with fabricated bytes.

### AR-005 | VSS host shadow + recover live disk

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Path: `vss_scanner.cpp` `HarddiskVolumeShadowCopy1..64`; recover `openDrive(driveIndex_)`
- Evidence: Enumerate not tied to evidence disk. Recover does not use VSS handle.
- Harm: Examiner machine C: files mix into case; "VSS recovery" reads live PhysicalDrive.

### AR-006 | BitLocker OEM string does not match real VBR

- Persona: Saboteur, Security Auditor, New Hire → **CRITICAL** (promoted)
- Path: `scan_coordinator.cpp` `memcmp(boot+3, "-FVEF-SYS-", 10)`; test same fake 10 bytes
- Evidence: BitLocker volume OEM 8 bytes `"-FVE-FS-"`. LBA 0 also protective MBR on GPT.
- Harm: Encrypted volume "not detected"; ciphertext carved/MFT mistaken. Test green lie.

### AR-007 | HFS B-tree `numRecords` heap OOB

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Path: `hfs_catalog.cpp` `walkCatalogNode` `offTable = blockSize - (numRecords+1)*2`
- Evidence: No `(numRecords+1)*2 <= blockSize`. 25k sentinel does not close this OOB.
- Harm: Hostile/corrupt HFS → admin process crash / memory corruption.

### AR-008 | `insertFilesBatch` swallows sqlite rc, always `true`

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Path: `metadata_store.cpp` `sqlite3_step` / `COMMIT` / `return true`
- Evidence: No per-step rc check. UI shows files via TSFN; DB silently incomplete.
- Harm: Case DB ≠ screen. `hfs_limit` sentinel may disappear.

### AR-009 | `ScanCoordinator` join/terminate

- Persona: Saboteur, New Hire → **CRITICAL**
- Path: `scan_coordinator.cpp` `stopScan` only `if (isRunning)`; worker end sets `isRunning=false` leaving joinable thread; `startScan` `scanThread = std::thread(...)` does not join old
- Evidence: C++ `std::thread` assignment while joinable → `std::terminate`.
- Harm: After first scan completes, second scan / dtor kills process. `StopScan` JS thread `join` + worker `BlockingCall` deadlock risk.

### AR-010 | Shared `Engine::diskReader_` race

- Persona: Saboteur → **CRITICAL**
- Path: `byteback_engine.h` single reader; `RecoverWorker` `openDrive`/`setRaidBackend`; Hex `readSectors`
- Evidence: No mutex. Recover closes handle while hex reads.
- Harm: AV / wrong sector / RAID backend stuck on hex.

### AR-011 | Report `status` lie + CoC paragraph

- Persona: Saboteur, New Hire, Security Auditor → **CRITICAL** (promoted)
- Path: `byteback_db.h` `0=Intact`; NTFS `IN_USE→1`; SQL `SUM(status=0)` `deletedFiles`; `ReportGenerator.tsx` labels these "Recoverable" and rest "Partially Overwritten". CoC: GENERIC_READ + "no writes except imaging" — `FILE_SHARE_WRITE`, wipe IPC, recover write.
- Harm: SHA-256 HTML/PDF fabricated overwrite stats and fake chain of custody.

### AR-012 | Resident NTFS / empty `runs` → MFT record dump

- Persona: Saboteur → **CRITICAL**
- Path: `recovery_engine.cpp` `runs.empty()` → `recoverCarvedFile` `sizeBytes` from `startSector`
- Evidence: Resident `$DATA` has no runs; startSector is MFT record. ADS resident same.
- Harm: "Recovered + MD5" is actually start of FILE record.

---

## Warnings

### AR-013 | RAID 6 no `fail_disk` in production; failed member zero

- Persona: Saboteur, New Hire
- Path: `virtual_raid.cpp` `fail_disk`; no NAPI call; `readMemberAligned` zero-fills, RS never engages
- Harm: "Dual parity" UI; single member I/O fail → zero stripe.

### AR-014 | Image `destPath` renderer string; open-fail silent

- Persona: Security Auditor, Saboteur
- Path: `ipc-handlers.ts` `start-imaging`; `disk_imager.cpp` fail → `isRunning_=false` no event
- Harm: Admin truncate; UI "Imaging…" dead worker.

### AR-015 | `sandbox: false` + `asar: false` + `requireAdministrator`

- Persona: Security Auditor
- Path: `main.ts` webPreferences; `package.json` electron-builder
- Harm: Writable install dir `.node`/renderer patch + UAC. Sandbox hard due to native addon; unpacked tree separate risk.

### AR-016 | Content FTS 256 KB cap; >16 MiB silent skip

- Persona: Saboteur, New Hire
- Path: `content_search.h` `maxBytesPerFile` / `maxFileSize`
- Harm: "Searched not found" false negative. 256 KB in UI; 16 MiB skip not shown.

### AR-017 | Official NSRL `NSRLFile.txt` SHA-1 → 0 hash, `ok=true`

- Persona: Security Auditor, New Hire
- Path: `nsrl_lookup.cpp` 32 hex; RDS column 1 SHA-1 40 hex
- Harm: Examiner "loaded, 0 hashes" or partial set "not in NSRL".

### AR-018 | APFS recover = superblock / 4096 B garbage

- Persona: New Hire, Saboteur
- Path: `apfs_container.cpp` volume `sizeBytes=blockSize`; Results `status` may look recoverable
- Harm: Label says "no catalog"; Recover still clickable.

### AR-019 | Audit log newline injection; chain resets on restart

- Persona: Security Auditor
- Path: `audit_logger.cpp` `LogEvent` no sanitize; `previousHash_` RAM
- Harm: Path/case notes fake EVENT line. "Tamper-evident" overstated.

### AR-020 | `searchFiles` regex ReDoS; count 1e6 RAM

- Persona: Saboteur, Security Auditor
- Path: `metadata_store.cpp` `std::regex(query)` LIMIT-less walk
- Harm: Examiner regex hangs elevated process.

### AR-021 | `destDir` no canonicalize; 10000th collision overwrite

- Persona: Security Auditor
- Path: `path_util.cpp` basename only; `uniqueDestPath` cap
- Harm: `destDir=\..\Windows\...`; prior recovery deleted.

### AR-022 | Hex I/O fail → 0x00 grid

- Persona: Saboteur
- Path: `ipc-handlers.ts` empty array; `HexEditor.tsx` pad 0
- Harm: Unreadable sector looks like evidence.

### AR-023 | Live list 5000 cap + page buttons lie

- Persona: Saboteur, New Hire
- Path: `App.tsx` cap; `ScanView.tsx` renders all `filesFound`
- Harm: Screen ≠ DB inventory.

### AR-024 | CSV formula injection

- Persona: Security Auditor
- Path: `ResultsView.tsx` `esc` does not neutralize `=`/`+`/`@`
- Harm: Excel DDE on forensic workstation.

### AR-025 | Report case fields no HTML escape

- Persona: Security Auditor
- Path: `ReportGenerator.tsx` investigator/caseNumber raw interpolation
- Harm: PDF hash taken before page Chromium executes.

### AR-026 | SMART hero "Drive Healthy" / Good

- Persona: Saboteur
- Path: `SmartView.tsx` large score; footnote UNCALIBRATED
- Harm: Label exists; triage decision still follows score.

---

## Notes

| Code | Topic |
|-----|------|
| AR-027 | Known ceilings (honest): no APFS omap, no `$LogFile` redo, HFS 25k banner, E01 4 GiB text, FTS 256 KB text, Weibull label, NSRL "not RDS", no BitLocker decrypt |
| AR-028 | INDX README "slack scan"; code path map, no slack FileRecord |
| AR-029 | USN timeline exists; `mftRef=0`, no MFT link |
| AR-030 | LZNT1 fail → raw compressed bytes + success |
| AR-031 | No `$ATTRIBUTE_LIST`; NTFS volume-wide FILE carve |
| AR-032 | `APP_VERSION` / `package.json` / `Engine::version_` three copies; Sidebar "PRO MAX" |
| AR-033 | No IPC validation tests (Vitest only shared helpers) |
| AR-034 | Google Fonts CDN (`index.css`) — airgap phone |

---

## Honest ceiling vs silent lie

Honest (examiner not misled if shown): no decrypt, no omap, no redo, E01 4 GiB *warning*, HFS 25k banner, 256 KB FTS text, Weibull footnote, in-memory NSRL.

Silent lie (court-facing surface): CoC, zero-fill success, status/overwrite counts, resident MD5, VSS host mix, BitLocker miss, E01 wrap then "complete", RAID 6 zero stripe, recover renderer runs.

## Merge rule

BLOCK lifts only after at least:

1. Recover `getFileById(scan_id, id)` — renderer `runs` ignored.
2. `startWipe` / image dest only main `dialog`; typed confirm for wipe.
3. E01 `currentChunkBytes_ > 0xffffffff` → `write`/`finish` false.
4. Zero-fill `success=false` or `zeroFilled/badSectors` examiner field.
5. BitLocker OEM `"-FVE-FS-"` + GPT volume boot; delete fake test vector.
6. HFS offset table bound; `insertFilesBatch` step rc; `ScanCoordinator` always join.
7. Report `status` dictionary single source; CoC documents `FILE_SHARE_WRITE` and no write-blocker.

## Test gaps (evidence)

Unit tests pass 146. Paths broken in production not green: E01 4 GiB, real FVE OEM, VSS recover, `insertFilesBatch` fail, hostile HFS `numRecords`, scan start-after-complete, recover renderer runs, wipe IPC.
