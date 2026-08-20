# Adversarial review — Byteback

- Tarih: 2026-08-19
- Kapsam: `byteback/` native + Electron (tam ürün, HEAD)
- Yöntem: üç zorunlu persona (Saboteur, New Hire, Security Auditor); 2+ persona = bir seviye terfi
- Runtime: statik kod okuma + önceki `ctest` 146/146 (birim testleri production teardown / 4 GiB E01 / gerçek FVE OEM kapsamıyor)
- Verdict: **BLOCK**

Önceki denetim (`2026-08-19.md`) Case/NSRL yüzeyi, HFS sentinel, SMART etiketi, README hizasını kapatmıştı. Bu koşu o yamaların *altında* kalan delil bütünlüğü, privilege IPC ve native crash/OOB yollarını açıyor.

## Özet sayıları

| Seviye | Adet |
|--------|------|
| CRITICAL | 12 |
| WARNING | 14 |
| NOTE | 8 |

## Persona notları

- **Saboteur:** üretimde tarama 2, `std::terminate`; HFS hostile volume OOB; recover sıfır + `success=true`; E01 wrap; shared `DiskReader`.
- **New Hire:** `status` üç anlama sahip (`byteback_db.h` Intact vs NTFS IN_USE vs rapor “üzerine yazılmış”); RAID 6 `fail_disk` NAPI’de yok; CoC paragrafı motorla çelişiyor.
- **Security Auditor:** `startWipe` hâlâ renderer string; recover `FileRecord.runs` renderer’dan; `requireAdministrator` + `sandbox: false` + `asar: false`; rapor HTML XSS.

---

## Critical Findings

### AR-001 | Recover renderer `runs` güveniyor — DB yok

- Persona: Security Auditor, Saboteur → **CRITICAL**
- Yol: `native/src/bridge/bridge_wipe.cpp` `FileRecordFromJs` / `RecoverFile`; `byteback_db.h` `getFileById` (kullanılmıyor)
- Kanıt: Recover, IPC nesnesindeki `runs[].startSector/sectorCount` ile okur. `scanId` yalnız `incrementRecovered`.
- Zarar: Admin süreç rastgele sektör aralığını “kurtarıldı + MD5” diye yazar.

### AR-002 | `startWipe` ham dosya yolu, teyit yok

- Persona: Security Auditor → **CRITICAL**
- Yol: `src/main/ipc-handlers.ts` `start-wipe`; `preload/index.ts`; `bridge_wipe.cpp` `StartWipe`
- Kanıt: Shredder UI disk wipe’ı kilitli. Preload `startWipe(targetPath)` açık. Native yalnız `PhysicalDrive` / `\\.\` reddeder.
- Zarar: Renderer (XSS/DevTools) dava dosyasını 3-pass yok eder.

### AR-003 | E01 >4 GiB native hâlâ yazar, tablo `uint32` wrap

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Yol: `ewf_writer.cpp` `write` `static_cast<uint32_t>(currentChunkBytes_)`; `finish()` true
- Kanıt: UI confirm var; writer reddetmez. Test yalnızca küçük sentetik.
- Zarar: Mahkeme E01 bozuk offset tablosu + “geçerli” MD5.

### AR-004 | Kötü sektör → sıfır, `success=true` / imaj tamamlandı

- Persona: Saboteur, Security Auditor, New Hire → **CRITICAL** (terfi)
- Yol: `recovery_engine.cpp` ~157–183; `disk_imager.cpp` ~84–96
- Kanıt: Okuma fail olunca sıfır yazılır, MD5 sıfırları içerir, sonuç başarılı.
- Zarar: Bit-for-bit / kurtarıldı iddiası uydurma bayt.

### AR-005 | VSS host gölgesi + recover live disk

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Yol: `vss_scanner.cpp` `HarddiskVolumeShadowCopy1..64`; recover `openDrive(driveIndex_)`
- Kanıt: Enumerate kanıt diske bağlı değil. Recover VSS handle kullanmaz.
- Zarar: Examiner makinesi C: dosyaları davaya karışır; “VSS kurtarma” canlı PhysicalDrive okur.

### AR-006 | BitLocker OEM string gerçek VBR ile eşleşmez

- Persona: Saboteur, Security Auditor, New Hire → **CRITICAL** (terfi)
- Yol: `scan_coordinator.cpp` `memcmp(boot+3, "-FVEF-SYS-", 10)`; test aynı sahte 10 byte
- Kanıt: BitLocker volume OEM 8 byte `"-FVE-FS-"`. LBA 0 ayrıca GPT’te protective MBR.
- Zarar: Şifreli birim “tespit edilmedi”; ciphertext carve/MFT sanılır. Test yeşil yalan.

### AR-007 | HFS B-tree `numRecords` heap OOB

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Yol: `hfs_catalog.cpp` `walkCatalogNode` `offTable = blockSize - (numRecords+1)*2`
- Kanıt: `(numRecords+1)*2 <= blockSize` yok. 25k sentinel bu OOB’u kapatmaz.
- Zarar: Hostile/bozuk HFS → admin süreç crash / bellek bozumu.

### AR-008 | `insertFilesBatch` sqlite rc yutar, hep `true`

- Persona: Saboteur, Security Auditor → **CRITICAL**
- Yol: `metadata_store.cpp` `sqlite3_step` / `COMMIT` / `return true`
- Kanıt: Adım rc kontrolü yok. UI TSFN ile dosya gösterir; DB sessiz eksik.
- Zarar: Dava DB ≠ ekran. `hfs_limit` sentinel kaybolabilir.

### AR-009 | `ScanCoordinator` join/terminate

- Persona: Saboteur, New Hire → **CRITICAL**
- Yol: `scan_coordinator.cpp` `stopScan` yalnız `if (isRunning)`; worker sonunda `isRunning=false` joinable thread bırakır; `startScan` `scanThread = std::thread(...)` eskiyi join etmez
- Kanıt: C++ `std::thread` atama joinable iken `std::terminate`.
- Zarar: İlk tarama bittikten sonra ikinci tarama / dtor süreç öldürür. `StopScan` JS thread’de `join` + worker `BlockingCall` deadlock riski.

### AR-010 | Paylaşımlı `Engine::diskReader_` ırk koşulu

- Persona: Saboteur → **CRITICAL**
- Yol: `byteback_engine.h` tek reader; `RecoverWorker` `openDrive`/`setRaidBackend`; Hex `readSectors`
- Kanıt: Mutex yok. Recover handle kapatırken hex okuyabilir.
- Zarar: AV / yanlış sektör / RAID backend’in hex’e yapışması.

### AR-011 | Rapor `status` yalanı + CoC paragrafı

- Persona: Saboteur, New Hire, Security Auditor → **CRITICAL** (terfi)
- Yol: `byteback_db.h` `0=Intact`; NTFS `IN_USE→1`; SQL `SUM(status=0)` `deletedFiles`; `ReportGenerator.tsx` bunu “Kurtarılabilir” ve kalanı “Kısmen Üzerine Yazılmış” yazar. CoC: GENERIC_READ + “imajlama dışında yazma yok” — `FILE_SHARE_WRITE`, wipe IPC, recover write.
- Zarar: SHA-256’lı HTML/PDF uydurma overwrite istatistiği ve sahte gözetim zinciri.

### AR-012 | Resident NTFS / boş `runs` → MFT kaydı dump

- Persona: Saboteur → **CRITICAL**
- Yol: `recovery_engine.cpp` `runs.empty()` → `recoverCarvedFile` `startSector`’dan `sizeBytes`
- Kanıt: Resident `$DATA` run yok; startSector MFT kaydı. ADS resident aynı.
- Zarar: “Kurtarıldı + MD5” aslında FILE kaydının başı.

---

## Warnings

### AR-013 | RAID 6 üretimde `fail_disk` yok; bozuk üye sıfır

- Persona: Saboteur, New Hire
- Yol: `virtual_raid.cpp` `fail_disk`; NAPI çağrısı yok; `readMemberAligned` sıfır basar, RS devreye girmez
- Zarar: “Çift parite” UI; tek üye I/O fail’de sıfır stripe.

### AR-014 | İmaj `destPath` renderer string; open-fail sessiz

- Persona: Security Auditor, Saboteur
- Yol: `ipc-handlers.ts` `start-imaging`; `disk_imager.cpp` fail → `isRunning_=false` eventsiz
- Zarar: Admin truncate; UI “İmaj Alınıyor…” ölü worker.

### AR-015 | `sandbox: false` + `asar: false` + `requireAdministrator`

- Persona: Security Auditor
- Yol: `main.ts` webPreferences; `package.json` electron-builder
- Zarar: Yazılabilir kurulum dizininde `.node`/renderer yaması + UAC. Native addon yüzünden sandbox zor; unpacked tree ayrı risk.

### AR-016 | Content FTS 256 KB kesik; 16 MiB üstü sessiz skip

- Persona: Saboteur, New Hire
- Yol: `content_search.h` `maxBytesPerFile` / `maxFileSize`
- Zarar: “Arandı bulunamadı” false negative. 256 KB UI’da var; 16 MiB skip yok.

### AR-017 | Resmi NSRL `NSRLFile.txt` SHA-1 → 0 hash, `ok=true`

- Persona: Security Auditor, New Hire
- Yol: `nsrl_lookup.cpp` 32 hex; RDS sütun 1 SHA-1 40 hex
- Zarar: Examiner “yüklendi, 0 hash” veya kısmi set ile “NSRL’de yok”.

### AR-018 | APFS recover = süperblok / 4096 B çöp

- Persona: New Hire, Saboteur
- Yol: `apfs_container.cpp` volume `sizeBytes=blockSize`; Results `status` kurtarılabilir görünebilir
- Zarar: Etiket “katalog yok” var; Recover hâlâ basılır.

### AR-019 | Audit log newline injection; zincir restart’ta sıfır

- Persona: Security Auditor
- Yol: `audit_logger.cpp` `LogEvent` sanitize yok; `previousHash_` RAM
- Zarar: Path/case notes sahte EVENT satırı. “Tamper-evident” abartı.

### AR-020 | `searchFiles` regex ReDoS; count 1e6 RAM

- Persona: Saboteur, Security Auditor
- Yol: `metadata_store.cpp` `std::regex(query)` LIMIT’siz walk
- Zarar: Examiner regex ile elevated süreç asması.

### AR-021 | `destDir` canonicalize yok; 10000. çakışmada overwrite

- Persona: Security Auditor
- Yol: `path_util.cpp` yalnız basename; `uniqueDestPath` tavanı
- Zarar: `destDir=\..\Windows\...`; önceki kurtarma silinir.

### AR-022 | Hex I/O fail → 0x00 ızgara

- Persona: Saboteur
- Yol: `ipc-handlers.ts` boş dizi; `HexEditor.tsx` pad 0
- Zarar: Okunamayan sektör delil gibi durur.

### AR-023 | Live liste 5000 cap + sayfa düğmeleri yalan

- Persona: Saboteur, New Hire
- Yol: `App.tsx` cap; `ScanView.tsx` tüm `filesFound` render
- Zarar: Ekran ≠ DB envanteri.

### AR-024 | CSV formula injection

- Persona: Security Auditor
- Yol: `ResultsView.tsx` `esc` `=`/`+`/`@` nötralize etmez
- Zarar: Excel DDE forensic iş istasyonunda.

### AR-025 | Rapor case alanları HTML escape yok

- Persona: Security Auditor
- Yol: `ReportGenerator.tsx` investigator/caseNumber ham interpolasyon
- Zarar: PDF hash, Chromium’un çalıştırdığı sayfadan önce alınır.

### AR-026 | SMART kahraman “Sürücü Sağlıklı” / Good

- Persona: Saboteur
- Yol: `SmartView.tsx` büyük skor; dipnot KALİBRASYONSUZ
- Zarar: Etiket var; triyaj kararı hâlâ skor.

---

## Notes

| Kod | Konu |
|-----|------|
| AR-027 | Bilinen tavanlar (dürüst): APFS omap yok, `$LogFile` redo yok, HFS 25k banner, E01 4 GiB metin, FTS 256 KB metin, Weibull etiket, NSRL “RDS değil”, BitLocker decrypt yok |
| AR-028 | INDX README “slack taraması”; kod path map, slack FileRecord yok |
| AR-029 | USN timeline var; `mftRef=0`, MFT bağ yok |
| AR-030 | LZNT1 fail → ham sıkışık bayt + success |
| AR-031 | `$ATTRIBUTE_LIST` yok; NTFS volume-wide FILE carve |
| AR-032 | `APP_VERSION` / `package.json` / `Engine::version_` üç kopya; Sidebar “PRO MAX” |
| AR-033 | IPC validation testleri yok (Vitest yalnız shared helpers) |
| AR-034 | Google Fonts CDN (`index.css`) — airgap telefon |

---

## Dürüst tavan vs sessiz yalan

Dürüst (examiner görürse aldanmaz): decrypt yok, omap yok, redo yok, E01 4 GiB *uyarısı*, HFS 25k banner, 256 KB FTS metni, Weibull dipnot, in-memory NSRL.

Sessiz yalan (mahkemeye giden yüzey): CoC, zero-fill success, status/overwrite sayıları, resident MD5, VSS host mix, BitLocker miss, E01 wrap sonrası “tamamlandı”, RAID 6 sıfır stripe, recover renderer runs.

## Merge kuralı

BLOCK kalkmaz ta ki en az:

1. Recover `getFileById(scan_id, id)` — renderer `runs` yok sayılır.
2. `startWipe` / imaj dest yalnız main `dialog`; wipe için typed confirm.
3. E01 `currentChunkBytes_ > 0xffffffff` → `write`/`finish` false.
4. Zero-fill `success=false` veya `zeroFilled/badSectors` examiner alanı.
5. BitLocker OEM `"-FVE-FS-"` + GPT volume boot; sahte test vektörü silinir.
6. HFS offset table bound; `insertFilesBatch` step rc; `ScanCoordinator` her zaman join.
7. Rapor `status` sözlüğü tek kaynak; CoC `FILE_SHARE_WRITE` ve write-blocker yokluğunu yazar.

## Test boşlukları (kanıt)

Birim testler 146 geçti. Production’da kırılan yollar yeşil değil: E01 4 GiB, gerçek FVE OEM, VSS recover, `insertFilesBatch` fail, hostile HFS `numRecords`, scan start-after-complete, recover renderer runs, wipe IPC.
