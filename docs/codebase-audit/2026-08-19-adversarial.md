# Adversarial review â€” Byteback

- Tarih: 2026-08-19
- Kapsam: `` native + Electron (tam Ã¼rÃ¼n, HEAD)
- YÃ¶ntem: Ã¼Ã§ zorunlu persona (Saboteur, New Hire, Security Auditor); 2+ persona = bir seviye terfi
- Runtime: statik kod okuma + Ã¶nceki `ctest` 146/146 (birim testleri production teardown / 4 GiB E01 / gerÃ§ek FVE OEM kapsamÄ±yor)
- Verdict: **BLOCK**

Ã–nceki denetim (`2026-08-19.md`) Case/NSRL yÃ¼zeyi, HFS sentinel, SMART etiketi, README hizasÄ±nÄ± kapatmÄ±ÅŸtÄ±. Bu koÅŸu o yamalarÄ±n *altÄ±nda* kalan delil bÃ¼tÃ¼nlÃ¼ÄŸÃ¼, privilege IPC ve native crash/OOB yollarÄ±nÄ± aÃ§Ä±yor.

## Ã–zet sayÄ±larÄ±

| Seviye | Adet |
|--------|------|
| CRITICAL | 12 |
| WARNING | 14 |
| NOTE | 8 |

## Persona notlarÄ±

- **Saboteur:** Ã¼retimde tarama 2, `std::terminate`; HFS hostile volume OOB; recover sÄ±fÄ±r + `success=true`; E01 wrap; shared `DiskReader`.
- **New Hire:** `status` Ã¼Ã§ anlama sahip (`byteback_db.h` Intact vs NTFS IN_USE vs rapor â€œÃ¼zerine yazÄ±lmÄ±ÅŸâ€); RAID 6 `fail_disk` NAPIâ€™de yok; CoC paragrafÄ± motorla Ã§eliÅŸiyor.
- **Security Auditor:** `startWipe` hÃ¢lÃ¢ renderer string; recover `FileRecord.runs` rendererâ€™dan; `requireAdministrator` + `sandbox: false` + `asar: false`; rapor HTML XSS.

---

## Critical Findings

### AR-001 | Recover renderer `runs` gÃ¼veniyor â€” DB yok

- Persona: Security Auditor, Saboteur â†’ **CRITICAL**
- Yol: `native/src/bridge/bridge_wipe.cpp` `FileRecordFromJs` / `RecoverFile`; `byteback_db.h` `getFileById` (kullanÄ±lmÄ±yor)
- KanÄ±t: Recover, IPC nesnesindeki `runs[].startSector/sectorCount` ile okur. `scanId` yalnÄ±z `incrementRecovered`.
- Zarar: Admin sÃ¼reÃ§ rastgele sektÃ¶r aralÄ±ÄŸÄ±nÄ± â€œkurtarÄ±ldÄ± + MD5â€ diye yazar.

### AR-002 | `startWipe` ham dosya yolu, teyit yok

- Persona: Security Auditor â†’ **CRITICAL**
- Yol: `src/main/ipc-handlers.ts` `start-wipe`; `preload/index.ts`; `bridge_wipe.cpp` `StartWipe`
- KanÄ±t: Shredder UI disk wipeâ€™Ä± kilitli. Preload `startWipe(targetPath)` aÃ§Ä±k. Native yalnÄ±z `PhysicalDrive` / `\\.\` reddeder.
- Zarar: Renderer (XSS/DevTools) dava dosyasÄ±nÄ± 3-pass yok eder.

### AR-003 | E01 >4 GiB native hÃ¢lÃ¢ yazar, tablo `uint32` wrap

- Persona: Saboteur, Security Auditor â†’ **CRITICAL**
- Yol: `ewf_writer.cpp` `write` `static_cast<uint32_t>(currentChunkBytes_)`; `finish()` true
- KanÄ±t: UI confirm var; writer reddetmez. Test yalnÄ±zca kÃ¼Ã§Ã¼k sentetik.
- Zarar: Mahkeme E01 bozuk offset tablosu + â€œgeÃ§erliâ€ MD5.

### AR-004 | KÃ¶tÃ¼ sektÃ¶r â†’ sÄ±fÄ±r, `success=true` / imaj tamamlandÄ±

- Persona: Saboteur, Security Auditor, New Hire â†’ **CRITICAL** (terfi)
- Yol: `recovery_engine.cpp` ~157â€“183; `disk_imager.cpp` ~84â€“96
- KanÄ±t: Okuma fail olunca sÄ±fÄ±r yazÄ±lÄ±r, MD5 sÄ±fÄ±rlarÄ± iÃ§erir, sonuÃ§ baÅŸarÄ±lÄ±.
- Zarar: Bit-for-bit / kurtarÄ±ldÄ± iddiasÄ± uydurma bayt.

### AR-005 | VSS host gÃ¶lgesi + recover live disk

- Persona: Saboteur, Security Auditor â†’ **CRITICAL**
- Yol: `vss_scanner.cpp` `HarddiskVolumeShadowCopy1..64`; recover `openDrive(driveIndex_)`
- KanÄ±t: Enumerate kanÄ±t diske baÄŸlÄ± deÄŸil. Recover VSS handle kullanmaz.
- Zarar: Examiner makinesi C: dosyalarÄ± davaya karÄ±ÅŸÄ±r; â€œVSS kurtarmaâ€ canlÄ± PhysicalDrive okur.

### AR-006 | BitLocker OEM string gerÃ§ek VBR ile eÅŸleÅŸmez

- Persona: Saboteur, Security Auditor, New Hire â†’ **CRITICAL** (terfi)
- Yol: `scan_coordinator.cpp` `memcmp(boot+3, "-FVEF-SYS-", 10)`; test aynÄ± sahte 10 byte
- KanÄ±t: BitLocker volume OEM 8 byte `"-FVE-FS-"`. LBA 0 ayrÄ±ca GPTâ€™te protective MBR.
- Zarar: Åifreli birim â€œtespit edilmediâ€; ciphertext carve/MFT sanÄ±lÄ±r. Test yeÅŸil yalan.

### AR-007 | HFS B-tree `numRecords` heap OOB

- Persona: Saboteur, Security Auditor â†’ **CRITICAL**
- Yol: `hfs_catalog.cpp` `walkCatalogNode` `offTable = blockSize - (numRecords+1)*2`
- KanÄ±t: `(numRecords+1)*2 <= blockSize` yok. 25k sentinel bu OOBâ€™u kapatmaz.
- Zarar: Hostile/bozuk HFS â†’ admin sÃ¼reÃ§ crash / bellek bozumu.

### AR-008 | `insertFilesBatch` sqlite rc yutar, hep `true`

- Persona: Saboteur, Security Auditor â†’ **CRITICAL**
- Yol: `metadata_store.cpp` `sqlite3_step` / `COMMIT` / `return true`
- KanÄ±t: AdÄ±m rc kontrolÃ¼ yok. UI TSFN ile dosya gÃ¶sterir; DB sessiz eksik.
- Zarar: Dava DB â‰  ekran. `hfs_limit` sentinel kaybolabilir.

### AR-009 | `ScanCoordinator` join/terminate

- Persona: Saboteur, New Hire â†’ **CRITICAL**
- Yol: `scan_coordinator.cpp` `stopScan` yalnÄ±z `if (isRunning)`; worker sonunda `isRunning=false` joinable thread bÄ±rakÄ±r; `startScan` `scanThread = std::thread(...)` eskiyi join etmez
- KanÄ±t: C++ `std::thread` atama joinable iken `std::terminate`.
- Zarar: Ä°lk tarama bittikten sonra ikinci tarama / dtor sÃ¼reÃ§ Ã¶ldÃ¼rÃ¼r. `StopScan` JS threadâ€™de `join` + worker `BlockingCall` deadlock riski.

### AR-010 | PaylaÅŸÄ±mlÄ± `Engine::diskReader_` Ä±rk koÅŸulu

- Persona: Saboteur â†’ **CRITICAL**
- Yol: `byteback_engine.h` tek reader; `RecoverWorker` `openDrive`/`setRaidBackend`; Hex `readSectors`
- KanÄ±t: Mutex yok. Recover handle kapatÄ±rken hex okuyabilir.
- Zarar: AV / yanlÄ±ÅŸ sektÃ¶r / RAID backendâ€™in hexâ€™e yapÄ±ÅŸmasÄ±.

### AR-011 | Rapor `status` yalanÄ± + CoC paragrafÄ±

- Persona: Saboteur, New Hire, Security Auditor â†’ **CRITICAL** (terfi)
- Yol: `byteback_db.h` `0=Intact`; NTFS `IN_USEâ†’1`; SQL `SUM(status=0)` `deletedFiles`; `ReportGenerator.tsx` bunu â€œKurtarÄ±labilirâ€ ve kalanÄ± â€œKÄ±smen Ãœzerine YazÄ±lmÄ±ÅŸâ€ yazar. CoC: GENERIC_READ + â€œimajlama dÄ±ÅŸÄ±nda yazma yokâ€ â€” `FILE_SHARE_WRITE`, wipe IPC, recover write.
- Zarar: SHA-256â€™lÄ± HTML/PDF uydurma overwrite istatistiÄŸi ve sahte gÃ¶zetim zinciri.

### AR-012 | Resident NTFS / boÅŸ `runs` â†’ MFT kaydÄ± dump

- Persona: Saboteur â†’ **CRITICAL**
- Yol: `recovery_engine.cpp` `runs.empty()` â†’ `recoverCarvedFile` `startSector`â€™dan `sizeBytes`
- KanÄ±t: Resident `$DATA` run yok; startSector MFT kaydÄ±. ADS resident aynÄ±.
- Zarar: â€œKurtarÄ±ldÄ± + MD5â€ aslÄ±nda FILE kaydÄ±nÄ±n baÅŸÄ±.

---

## Warnings

### AR-013 | RAID 6 Ã¼retimde `fail_disk` yok; bozuk Ã¼ye sÄ±fÄ±r

- Persona: Saboteur, New Hire
- Yol: `virtual_raid.cpp` `fail_disk`; NAPI Ã§aÄŸrÄ±sÄ± yok; `readMemberAligned` sÄ±fÄ±r basar, RS devreye girmez
- Zarar: â€œÃ‡ift pariteâ€ UI; tek Ã¼ye I/O failâ€™de sÄ±fÄ±r stripe.

### AR-014 | Ä°maj `destPath` renderer string; open-fail sessiz

- Persona: Security Auditor, Saboteur
- Yol: `ipc-handlers.ts` `start-imaging`; `disk_imager.cpp` fail â†’ `isRunning_=false` eventsiz
- Zarar: Admin truncate; UI â€œÄ°maj AlÄ±nÄ±yorâ€¦â€ Ã¶lÃ¼ worker.

### AR-015 | `sandbox: false` + `asar: false` + `requireAdministrator`

- Persona: Security Auditor
- Yol: `main.ts` webPreferences; `package.json` electron-builder
- Zarar: YazÄ±labilir kurulum dizininde `.node`/renderer yamasÄ± + UAC. Native addon yÃ¼zÃ¼nden sandbox zor; unpacked tree ayrÄ± risk.

### AR-016 | Content FTS 256 KB kesik; 16 MiB Ã¼stÃ¼ sessiz skip

- Persona: Saboteur, New Hire
- Yol: `content_search.h` `maxBytesPerFile` / `maxFileSize`
- Zarar: â€œArandÄ± bulunamadÄ±â€ false negative. 256 KB UIâ€™da var; 16 MiB skip yok.

### AR-017 | Resmi NSRL `NSRLFile.txt` SHA-1 â†’ 0 hash, `ok=true`

- Persona: Security Auditor, New Hire
- Yol: `nsrl_lookup.cpp` 32 hex; RDS sÃ¼tun 1 SHA-1 40 hex
- Zarar: Examiner â€œyÃ¼klendi, 0 hashâ€ veya kÄ±smi set ile â€œNSRLâ€™de yokâ€.

### AR-018 | APFS recover = sÃ¼perblok / 4096 B Ã§Ã¶p

- Persona: New Hire, Saboteur
- Yol: `apfs_container.cpp` volume `sizeBytes=blockSize`; Results `status` kurtarÄ±labilir gÃ¶rÃ¼nebilir
- Zarar: Etiket â€œkatalog yokâ€ var; Recover hÃ¢lÃ¢ basÄ±lÄ±r.

### AR-019 | Audit log newline injection; zincir restartâ€™ta sÄ±fÄ±r

- Persona: Security Auditor
- Yol: `audit_logger.cpp` `LogEvent` sanitize yok; `previousHash_` RAM
- Zarar: Path/case notes sahte EVENT satÄ±rÄ±. â€œTamper-evidentâ€ abartÄ±.

### AR-020 | `searchFiles` regex ReDoS; count 1e6 RAM

- Persona: Saboteur, Security Auditor
- Yol: `metadata_store.cpp` `std::regex(query)` LIMITâ€™siz walk
- Zarar: Examiner regex ile elevated sÃ¼reÃ§ asmasÄ±.

### AR-021 | `destDir` canonicalize yok; 10000. Ã§akÄ±ÅŸmada overwrite

- Persona: Security Auditor
- Yol: `path_util.cpp` yalnÄ±z basename; `uniqueDestPath` tavanÄ±
- Zarar: `destDir=\..\Windows\...`; Ã¶nceki kurtarma silinir.

### AR-022 | Hex I/O fail â†’ 0x00 Ä±zgara

- Persona: Saboteur
- Yol: `ipc-handlers.ts` boÅŸ dizi; `HexEditor.tsx` pad 0
- Zarar: Okunamayan sektÃ¶r delil gibi durur.

### AR-023 | Live liste 5000 cap + sayfa dÃ¼ÄŸmeleri yalan

- Persona: Saboteur, New Hire
- Yol: `App.tsx` cap; `ScanView.tsx` tÃ¼m `filesFound` render
- Zarar: Ekran â‰  DB envanteri.

### AR-024 | CSV formula injection

- Persona: Security Auditor
- Yol: `ResultsView.tsx` `esc` `=`/`+`/`@` nÃ¶tralize etmez
- Zarar: Excel DDE forensic iÅŸ istasyonunda.

### AR-025 | Rapor case alanlarÄ± HTML escape yok

- Persona: Security Auditor
- Yol: `ReportGenerator.tsx` investigator/caseNumber ham interpolasyon
- Zarar: PDF hash, Chromiumâ€™un Ã§alÄ±ÅŸtÄ±rdÄ±ÄŸÄ± sayfadan Ã¶nce alÄ±nÄ±r.

### AR-026 | SMART kahraman â€œSÃ¼rÃ¼cÃ¼ SaÄŸlÄ±klÄ±â€ / Good

- Persona: Saboteur
- Yol: `SmartView.tsx` bÃ¼yÃ¼k skor; dipnot KALÄ°BRASYONSUZ
- Zarar: Etiket var; triyaj kararÄ± hÃ¢lÃ¢ skor.

---

## Notes

| Kod | Konu |
|-----|------|
| AR-027 | Bilinen tavanlar (dÃ¼rÃ¼st): APFS omap yok, `$LogFile` redo yok, HFS 25k banner, E01 4 GiB metin, FTS 256 KB metin, Weibull etiket, NSRL â€œRDS deÄŸilâ€, BitLocker decrypt yok |
| AR-028 | INDX README â€œslack taramasÄ±â€; kod path map, slack FileRecord yok |
| AR-029 | USN timeline var; `mftRef=0`, MFT baÄŸ yok |
| AR-030 | LZNT1 fail â†’ ham sÄ±kÄ±ÅŸÄ±k bayt + success |
| AR-031 | `$ATTRIBUTE_LIST` yok; NTFS volume-wide FILE carve |
| AR-032 | `APP_VERSION` / `package.json` / `Engine::version_` Ã¼Ã§ kopya; Sidebar â€œPRO MAXâ€ |
| AR-033 | IPC validation testleri yok (Vitest yalnÄ±z shared helpers) |
| AR-034 | Google Fonts CDN (`index.css`) â€” airgap telefon |

---

## DÃ¼rÃ¼st tavan vs sessiz yalan

DÃ¼rÃ¼st (examiner gÃ¶rÃ¼rse aldanmaz): decrypt yok, omap yok, redo yok, E01 4 GiB *uyarÄ±sÄ±*, HFS 25k banner, 256 KB FTS metni, Weibull dipnot, in-memory NSRL.

Sessiz yalan (mahkemeye giden yÃ¼zey): CoC, zero-fill success, status/overwrite sayÄ±larÄ±, resident MD5, VSS host mix, BitLocker miss, E01 wrap sonrasÄ± â€œtamamlandÄ±â€, RAID 6 sÄ±fÄ±r stripe, recover renderer runs.

## Merge kuralÄ±

BLOCK kalkmaz ta ki en az:

1. Recover `getFileById(scan_id, id)` â€” renderer `runs` yok sayÄ±lÄ±r.
2. `startWipe` / imaj dest yalnÄ±z main `dialog`; wipe iÃ§in typed confirm.
3. E01 `currentChunkBytes_ > 0xffffffff` â†’ `write`/`finish` false.
4. Zero-fill `success=false` veya `zeroFilled/badSectors` examiner alanÄ±.
5. BitLocker OEM `"-FVE-FS-"` + GPT volume boot; sahte test vektÃ¶rÃ¼ silinir.
6. HFS offset table bound; `insertFilesBatch` step rc; `ScanCoordinator` her zaman join.
7. Rapor `status` sÃ¶zlÃ¼ÄŸÃ¼ tek kaynak; CoC `FILE_SHARE_WRITE` ve write-blocker yokluÄŸunu yazar.

## Test boÅŸluklarÄ± (kanÄ±t)

Birim testler 146 geÃ§ti. Productionâ€™da kÄ±rÄ±lan yollar yeÅŸil deÄŸil: E01 4 GiB, gerÃ§ek FVE OEM, VSS recover, `insertFilesBatch` fail, hostile HFS `numRecords`, scan start-after-complete, recover renderer runs, wipe IPC.
