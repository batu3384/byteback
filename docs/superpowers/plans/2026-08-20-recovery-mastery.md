# Byteback â€” Tarama & Kurtarma UstalÄ±ÄŸÄ± PlanÄ±

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Byteback'yi ticari veri kurtarma yazÄ±lÄ±mlarÄ±na (R-Studio, Recuva, Disk Drill, PhotoRec) yaklaÅŸtÄ±rmak; ana odak disk tarama, silinmiÅŸ veri bulma geniÅŸliÄŸi ve kurtarma yetkinliÄŸi.

**Architecture:** Mevcut `ScanCoordinator` â†’ FS parser'lar â†’ `CarvingEngine` â†’ `RecoveryEngine` hattÄ±nÄ± geniÅŸlet; tarama kapsamÄ±nÄ± (partition/volume/unallocated), metadata derinliÄŸini (NTFS path rebuild, ReFS), carving kapsamÄ±nÄ± (imza + Ã¶zel parser) ve kurtarma kalitesini (preview, doÄŸrulama, dedup) katman katman yÃ¼kselt.

**Tech Stack:** C++17 native motor, Electron/React UI, SQLite metadata, GoogleTest + Vitest + Playwright.

**Spec:** Bu plan doÄŸrudan kullanÄ±cÄ± gereksiniminden tÃ¼retildi; ayrÄ± spec dosyasÄ± `docs/superpowers/specs/2026-08-20-recovery-mastery-design.md` onay sonrasÄ± yazÄ±lacak.

## Global Constraints

- Windows-only native I/O (`\\.\PhysicalDriveN`, `DeviceIoControl`).
- Motor kaynak diske `GENERIC_READ`; yazma yalnÄ±z kullanÄ±cÄ± hedef yoluna.
- BitLocker bypass yok; FVEK veya recovery password ÅŸart.
- SSD TRIM sonrasÄ± kurtarma fiziksel olarak imkansÄ±z â€” UI bunu aÃ§Ä±k sÃ¶ylemeli.
- GPU/CUDA PFAC ilk fazlarda yok; CPU paralelleÅŸtirme Ã¶ncelikli.
- Her faz sonunda `ctest -C Release` + `npm run test` yeÅŸil kalmalÄ±.

---

## Mevcut Durum Ã–zeti (AÄŸustos 2026)

### GÃ¼Ã§lÃ¼ yanlar (korunacak)
- NTFS: MFT run walk, silinen kayÄ±t (`status=0`), orphan FILE carve (deep), USN timeline, INDX slack, $LogFile hints, LZNT1 kurtarma.
- FAT/exFAT: zincir yÃ¼rÃ¼yÃ¼ÅŸÃ¼, silinmiÅŸ 0xE5, VFAT.
- ext4: extent tree, silinmiÅŸ inode/dirent.
- Carving: Aho-Corasick ~114 imza, FOV validator, BGC (sektÃ¶r adÄ±mlÄ±, bÃ¼tÃ§eli).
- BitLocker FVEK + clear-key recovery password, VSS, RAID 0/1/5/6/10.
- Audit SHA-256 zinciri testli; kÃ¶tÃ¼ sektÃ¶r telemetrisi UI'ya baÄŸlÄ±.

### Kritik eksiklikler (ticari araÃ§lara gÃ¶re)
| Alan | Byteback bugÃ¼n | R-Studio / PhotoRec / Recuva |
|------|-----------|------------------------------|
| Tarama hedefi | TÃ¼m fiziksel disk | Partition / volume / unallocated seÃ§imi |
| SÃ¼rÃ¼cÃ¼ harfi (D:) | Yok | Var |
| Carve kapsamÄ± | TÃ¼m disk sektÃ¶rleri | Ã‡oÄŸunlukla boÅŸ kÃ¼me / unallocated |
| Ä°mza sayÄ±sÄ± | ~114 | 300â€“400+ |
| NTFS path rebuild | KÄ±smi (MFT parent walk) | Tam dizin aÄŸacÄ± + $LogFile |
| ReFS | Yok | Var |
| Paralel tarama | Tek thread | Ã‡ok Ã§ekirdek |
| Ã–nizleme | Yok | Resim/PDF/text |
| Dedup | Yok | MFT+carve birleÅŸtirme |
| Office/SQLite/Video | Yok | Ã–zel parser |
| E2E kurtarma testi | Duman testi | Senaryo testleri |

---

## Faz 0 â€” Tarama AltyapÄ±sÄ± & UX Temeli (P0)

**AmaÃ§:** KullanÄ±cÄ± D: gibi bir birimi doÄŸru hedefleyebilsin; carve gereksiz yere tÃ¼m diski taramasÄ±n.

### Task 0.1: Volume / partition hedefli tarama

**Files:**
- Modify: `native/include/scan_coordinator.h`
- Modify: `native/src/scan_coordinator.cpp`
- Modify: `native/src/bridge/bridge_scan.cpp`
- Modify: `src/main/ipc-handlers.ts`, `src/preload/index.ts`, `src/shared/types.ts`
- Modify: `src/renderer/components/Dashboard/DriveCard.tsx`, `src/renderer/App.tsx`

**Interfaces:**
- Consumes: mevcut `runQuickScan`, `runDeepScan`, `runCarveScan`
- Produces:
  ```cpp
  struct ScanTarget {
      int driveIndex;
      int64_t partitionStartSector = -1; // -1 = tÃ¼m disk
      uint64_t partitionSizeSectors = 0;
      bool carveUnallocatedOnly = false;
  };
  void ScanCoordinator::startScan(const ScanTarget& target, const std::string& scanType, ...);
  ```

- [ ] **Step 1:** `ScanTarget` struct + `DiskReader::setReadBounds(startSector, sectorCount)` ekle (okuma clamp).
- [ ] **Step 2:** `runQuickScan` partition offset/size ile sÄ±nÄ±rla; carve aynÄ± bounds iÃ§inde kalsÄ±n.
- [ ] **Step 3:** NAPI `startScan(driveIndex, scanType, opts?)` â€” `partitionIndex` veya `startSector`+`sizeSectors`.
- [ ] **Step 4:** UI: `listPartitions` API'sini DriveCard'da kullan; partition seÃ§ici dropdown.
- [ ] **Step 5:** Test: sentetik GPT gÃ¶rÃ¼ntÃ¼sÃ¼nde yalnÄ±zca 2. partition taranÄ±r.

### Task 0.2: SÃ¼rÃ¼cÃ¼ harfi â†’ fiziksel disk eÅŸlemesi

**Files:**
- Create: `native/src/io/volume_mapper_win.cpp`, `native/include/io/volume_mapper_win.h`
- Modify: `native/src/bridge/bridge_drives.cpp`

**Interfaces:**
- Produces: `std::optional<ScanTarget> resolveDriveLetter(const std::wstring& letter); // e.g. L"D:"`

- [ ] **Step 1:** `QueryDosDeviceW` + `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` ile harf â†’ PhysicalDrive + offset.
- [ ] **Step 2:** NAPI `resolveVolume("D:")` â†’ `{ driveIndex, startSector, sizeSectors, fsType }`.
- [ ] **Step 3:** UI: "D: tara" hÄ±zlÄ± kÄ±sayolu Dashboard'da.
- [ ] **Step 4:** Test: mock IOCTL veya entegrasyon (test ortamÄ±nda mevcut birim varsa).

### Task 0.3: Unallocated-only carve modu

**Files:**
- Create: `native/src/fs/unallocated_map.cpp`, `native/include/fs/unallocated_map.h`
- Modify: `native/src/scan_coordinator.cpp`, `native/src/carver/signature_engine.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct SectorRange { uint64_t start; uint64_t count; };
  std::vector<SectorRange> buildUnallocatedRanges(DiskReader&, VolumeFsKind, uint64_t volOffset, uint64_t volSize);
  ```

- [ ] **Step 1:** NTFS: `$Bitmap` okuyarak free cluster â†’ sektÃ¶r aralÄ±klarÄ±.
- [ ] **Step 2:** FAT: FAT tablosundan free cluster zinciri dÄ±ÅŸÄ± aralÄ±klar.
- [ ] **Step 3:** `CarvingEngine::scanRanges(reader, ranges, ...)` â€” mevcut `scan`'i refactor.
- [ ] **Step 4:** Deep scan varsayÄ±lan: metadata full partition + carve unallocated only.
- [ ] **Step 5:** Test: NTFS gÃ¶rÃ¼ntÃ¼sÃ¼nde allocated bÃ¶lgede carve adayÄ± Ã¼retilmez.

### Task 0.4: Tarama profilleri & dÃ¼rÃ¼st etiketler

**Files:**
- Modify: `src/renderer/components/ScanView/ScanView.tsx`, `DriveCard.tsx`

- [ ] **Step 1:** ÃœÃ§ mod: **HÄ±zlÄ±** (metadata, orphan kapalÄ±), **Derin** (metadata + unallocated carve), **Tam Disk Carve** (eski davranÄ±ÅŸ, uyarÄ± ile).
- [ ] **Step 2:** SSD tespit edilince scan baÅŸlamadan TRIM uyarÄ±sÄ± modal.
- [ ] **Step 3:** Playwright: mod seÃ§imi + uyarÄ± metni smoke.

**Faz 0 Ã§Ä±kÄ±ÅŸ kriteri:** D: harfi veya partition seÃ§ilerek deep scan; carve yalnÄ±z boÅŸ alan; testler yeÅŸil.

---

## Faz 1 â€” NTFS Metadata DerinliÄŸi (P0)

**AmaÃ§:** SilinmiÅŸ dosyalarda isim + klasÃ¶r yolu oranÄ±nÄ± R-Studio'ya yaklaÅŸtÄ±r.

### Task 1.1: Tam $MFT kapsamÄ±

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`

- [ ] **Step 1:** Boot `$MFT` run yÃ¼rÃ¼yÃ¼ÅŸÃ¼ne ek olarak `$MFT::$DATA` attribute runs ile tÃ¼m MFT extent'leri tara.
- [ ] **Step 2:** MFT fragmentasyonu (non-contiguous $MFT) sentetik test gÃ¶rÃ¼ntÃ¼sÃ¼.
- [ ] **Step 3:** Orphan carve ile Ã§akÄ±ÅŸan kayÄ±tlarÄ± MFT record numarasÄ±na gÃ¶re birleÅŸtir.

### Task 1.2: Dizin aÄŸacÄ± yeniden kurulumu

**Files:**
- Create: `native/src/fs/ntfs_path_rebuild.cpp`, `native/include/fs/ntfs_path_rebuild.h`
- Modify: `native/src/fs/ntfs_parser.cpp`

**Interfaces:**
- Produces: `std::string rebuildPath(uint64_t mftRecord, const MftIndex& idx);`

- [ ] **Step 1:** TÃ¼m FILE record'lardan parent MFT ref + isim topla.
- [ ] **Step 2:** SilinmiÅŸ parent'lar iÃ§in `$LogFile` + INDX slack hint'lerini dÃ¼ÅŸÃ¼k gÃ¼ven skoruyla baÄŸla.
- [ ] **Step 3:** `fr.path` ve `fr.name` alanlarÄ±nÄ± tam yol olarak doldur (`/Users/foo/bar.jpg`).
- [ ] **Step 4:** Test: silinmiÅŸ alt dizin + dosya senaryosu.

### Task 1.3: $LogFile â†’ kurtarÄ±labilir kayÄ±t yÃ¼kseltme

**Files:**
- Modify: `native/src/fs/ntfs_logfile.cpp`, `ntfs_parser.cpp`

- [ ] **Step 1:** LogFile'dan Ã§Ä±kan isimleri MFT record ID ile eÅŸleÅŸtir (LSN/transaction hint).
- [ ] **Step 2:** EÅŸleÅŸen kayÄ±tlarÄ±n `confidence` alanÄ±nÄ± yÃ¼kselt; UI'da "LogFile doÄŸrulandÄ±" etiketi.
- [ ] **Step 3:** Test: `test_ntfs_logfile.cpp` geniÅŸlet.

### Task 1.4: USN â†’ silme olayÄ± filtreleme

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`, `src/renderer/components/ResultsView/ResultsView.tsx`

- [ ] **Step 1:** USN kayÄ±tlarÄ±nÄ± timeline'da tut; ayrÄ± "kurtarÄ±labilir" listesine karÄ±ÅŸtÄ±rma.
- [ ] **Step 2:** ResultsView'da "YalnÄ±z silinmiÅŸ" filtresi (`status === 0`).
- [ ] **Step 3:** Test: Vitest filtre birimi.

**Faz 1 Ã§Ä±kÄ±ÅŸ kriteri:** NTFS silinmiÅŸ dosyalarda %80+ tam yol (sentetik golden image setinde).

---

## Faz 2 â€” Carving GeniÅŸliÄŸi & HÄ±z (P1)

**AmaÃ§:** PhotoRec seviyesine yaklaÅŸan format kapsamÄ± ve kabul edilebilir hÄ±z.

### Task 2.1: Ä°mza kÃ¼tÃ¼phanesi geniÅŸletme

**Files:**
- Modify: `native/src/carver/signature_engine.cpp` (embedded table)
- Create: `resources/signatures-extended.json`
- Create: `native/tests/test_signature_coverage.cpp`

- [ ] **Step 1:** PhotoRec / file(1) magic listesinden 200+ yeni imza ekle (Ã¶ncelik: jpg, png, mp4, mov, docx, xlsx, pptx, zip, rar, 7z, pdf, sqlite, pst, eml, wav, mp3, heic, cr2, nef, orf, arw).
- [ ] **Step 2:** Her imza iÃ§in FOV validator veya footer zorunluluÄŸu belirle.
- [ ] **Step 3:** Test: bilinen magic â†’ doÄŸru uzantÄ±.

### Task 2.2: Ã–zel yapÄ±sal parser'lar

**Files:**
- Create: `native/src/carver/parsers/zip_family.cpp`, `sqlite_carver.cpp`, `mp4_carver.cpp`
- Modify: `native/src/carver/signature_engine.cpp`

- [ ] **Step 1:** ZIP tabanlÄ± Office (docx/xlsx/pptx): central directory scan.
- [ ] **Step 2:** SQLite: header + schema page validation, boyut tahmini.
- [ ] **Step 3:** MP4/MOV: `ftyp` + `moov`/`mdat` atom walk; moov sonda ise tail search.
- [ ] **Step 4:** Test: her parser iÃ§in minimal binary fixture.

### Task 2.3: CPU paralel carve

**Files:**
- Modify: `native/src/carver/signature_engine.cpp`
- Create: `native/include/util/thread_pool.h` (minimal, ponytail: std::async veya fixed pool)

- [ ] **Step 1:** Chunk listesini N worker'a bÃ¶l; sonuÃ§larÄ± mutex'li birleÅŸtir.
- [ ] **Step 2:** `std::atomic` progress birleÅŸtirme.
- [ ] **Step 3:** Benchmark test: 1 GB sentetik imaj, 4 thread vs 1 thread.

### Task 2.4: MFT â†” carve deduplikasyon

**Files:**
- Create: `native/src/scan/dedup_index.cpp`
- Modify: `native/src/bridge/bridge_scan.cpp`, `native/src/db/metadata_store.cpp`

- [ ] **Step 1:** `(startSector, size, hash prefix)` ile carve/MFT Ã§akÄ±ÅŸmasÄ±nÄ± tespit.
- [ ] **Step 2:** YÃ¼ksek gÃ¼venli MFT kaydÄ±nÄ± tut; carve kopyasÄ±nÄ± `duplicate_of` ile iÅŸaretle.
- [ ] **Step 3:** UI: "TekrarlarÄ± gizle" toggle.

**Faz 2 Ã§Ä±kÄ±ÅŸ kriteri:** 250+ imza; 4 Ã§ekirdekte â‰¥2Ã— hÄ±z; Office/SQLite/MP4 carve Ã§alÄ±ÅŸÄ±r.

---

## Faz 3 â€” DiÄŸer Dosya Sistemleri (P1)

### Task 3.1: ReFS parser (Windows Server / Win10+)

**Files:**
- Create: `native/src/fs/refs_parser.cpp`, `native/include/fs/refs_parser.h`
- Modify: `native/src/fs/partition_scanner.cpp`, `scan_coordinator.cpp`

- [ ] **Step 1:** ReFS sÃ¼perblok + container tablosu tanÄ±ma.
- [ ] **Step 2:** Directory B+ tree walk, silinmiÅŸ entry.
- [ ] **Step 3:** Test: ReFS test gÃ¶rÃ¼ntÃ¼sÃ¼ (veya minimal fixture).

### Task 3.2: exFAT silinmiÅŸ entry iyileÅŸtirme

**Files:**
- Modify: `native/src/fs/fat_parser.cpp`

- [ ] **Step 1:** Entry-set fragmentation (non-contiguous name entries) toleransÄ±.
- [ ] **Step 2:** SilinmiÅŸ + allocated karÄ±ÅŸÄ±k dizin taramasÄ±.

### Task 3.3: APFS katalog derinliÄŸi

**Files:**
- Modify: `native/src/fs/apfs_container.cpp`, `apfs_parser.cpp`

- [ ] **Step 1:** Snapshot tree walk (dokÃ¼mante sÄ±nÄ±r kaldÄ±rma).
- [ ] **Step 2:** `apfs_file` keÅŸif â†’ `runs` doldurma oranÄ±nÄ± artÄ±r.
- [ ] **Step 3:** Test: mevcut APFS fixture geniÅŸlet.

**Faz 3 Ã§Ä±kÄ±ÅŸ kriteri:** ReFS volume tanÄ±nÄ±r ve dosya listeler; APFS kurtarÄ±labilir dosya oranÄ± artar.

---

## Faz 4 â€” Kurtarma Kalitesi & DoÄŸrulama (P1)

### Task 4.1: Kurtarma Ã¶nizleme

**Files:**
- Create: `native/src/recovery/preview_reader.cpp`
- Modify: `src/renderer/components/ResultsView/ResultsView.tsx`
- Modify: `src/main/ipc-handlers.ts`

- [ ] **Step 1:** Ä°lk 64 KB okuma NAPI `readFilePreview(scanId, fileId)`.
- [ ] **Step 2:** UI: resim thumbnail (jpg/png/gif), text hex+ascii, PDF ilk sayfa (basit).
- [ ] **Step 3:** Test: Vitest preview API mock.

### Task 4.2: Kurtarma sonrasÄ± doÄŸrulama

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`

- [ ] **Step 1:** KurtarÄ±lan dosya iÃ§in validator tekrar Ã§alÄ±ÅŸtÄ±r (carve kaynaklÄ±).
- [ ] **Step 2:** `RecoveryResult.validationScore` + `validationError`.
- [ ] **Step 3:** UI: "Bozuk / Tam" sÃ¼tunu.

### Task 4.3: ParÃ§alÄ± dosya birleÅŸtirme (Ã§oklu run)

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`, `bgc.cpp`

- [ ] **Step 1:** BGC `frag1 + gap + frag2` Ã§Ä±ktÄ±sÄ±nÄ± `runs` vektÃ¶rÃ¼ne yaz (mevcut kÄ±smen var â€” tamamla).
- [ ] **Step 2:** 3+ parÃ§a iÃ§in sÄ±nÄ±rlÄ± brute (ponytail: max 3 gap, bÃ¼tÃ§e).
- [ ] **Step 3:** Test: iki parÃ§alÄ± JPEG golden.

### Task 4.4: BitLocker geniÅŸletme (ÅŸifre koruyucu)

**Files:**
- Modify: `native/src/fs/bitlocker_unlock.cpp`

- [ ] **Step 1:** Password protector (VMK decrypt) â€” libmsbde veya minimal PBKDF2 implementasyonu.
- [ ] **Step 2:** TPM/startup-key iÃ§in aÃ§Ä±k hata mesajÄ± koru.
- [ ] **Step 3:** Test: bilinen test vector (Microsoft dokÃ¼mantasyonu).

**Faz 4 Ã§Ä±kÄ±ÅŸ kriteri:** KullanÄ±cÄ± kurtarmadan Ã¶nce Ã¶nizler; bozuk dosya oranÄ± raporlanÄ±r.

---

## Faz 5 â€” GÃ¼venilirlik, Test & Benchmark (P2)

### Task 5.1: Golden image regression suite

**Files:**
- Create: `native/tests/fixtures/ntfs_deleted/`, `native/tests/test_recovery_golden.cpp`
- Create: `scripts/generate-fixtures.ps1`

- [ ] **Step 1:** NTFS: 10 silinmiÅŸ dosya (txt, jpg, docx, parÃ§alÄ± jpg).
- [ ] **Step 2:** FAT32: 5 silinmiÅŸ dosya.
- [ ] **Step 3:** CI: `ctest -R GoldenRecovery` â€” bulunan / kurtarÄ±lan oran raporu.

### Task 5.2: E2E kurtarma akÄ±ÅŸÄ±

**Files:**
- Modify: `e2e/electron-smoke.spec.ts`
- Create: `e2e/recovery-flow.spec.ts`

- [ ] **Step 1:** Mock native veya test disk ile scan â†’ result â†’ recover.
- [ ] **Step 2:** MD5 doÄŸrulama.

### Task 5.3: Tarama checkpoint / resume

**Files:**
- Modify: `native/src/scan_coordinator.cpp`, `metadata_store.cpp`

- [ ] **Step 1:** Son taranan sektÃ¶r SQLite'a yaz.
- [ ] **Step 2:** Kesilen taramayÄ± "Devam et" ile sÃ¼rdÃ¼r.
- [ ] **Step 3:** Test: yarÄ±da kes + resume.

### Task 5.4: Performans profili

**Files:**
- Create: `native/tests/bench_scan.cpp` (gtest benchmark veya basit cronometre)

- [ ] **Step 1:** 500 GB simÃ¼le (sparse file) deep scan sÃ¼re raporu.
- [ ] **Step 2:** README'ye honest benchmark tablosu.

**Faz 5 Ã§Ä±kÄ±ÅŸ kriteri:** Golden testler CI'da; E2E recovery pass; resume Ã§alÄ±ÅŸÄ±r.

---

## Faz 6 â€” Ä°leri (P3, isteÄŸe baÄŸlÄ±)

| Task | AÃ§Ä±klama | Not |
|------|----------|-----|
| 6.1 GPU PFAC | CUDA/OpenCL Aho-Corasick | BÃ¼yÃ¼k yatÄ±rÄ±m; Faz 2 paralel CPU yetmezse |
| 6.2 ReFS integrity stream | ReFS checksum doÄŸrulama | Server senaryosu |
| 6.3 Cloud / aÄŸ imaj | E01 over network | Adli senaryo |
| 6.4 macOS / Linux I/O | `disk_reader_posix.cpp` | Platform geniÅŸleme |
| 6.5 AI dosya sÄ±nÄ±flandÄ±rma | Entropi + ML kategori | Pazarlama deÄŸil, gerÃ§ek deÄŸer ÅŸÃ¼pheli |

---

## Ã–ncelik SÄ±rasÄ± (Uygulama SÄ±rasÄ±)

```
Faz 0 (P0) â†’ Faz 1 (P0) â†’ Faz 2 (P1) â†’ Faz 4.1-4.3 (P1) â†’ Faz 3 (P1) â†’ Faz 4.4 (P2) â†’ Faz 5 (P2) â†’ Faz 6 (P3)
```

**GerekÃ§e:** Ã–nce doÄŸru hedef (D:/partition) + unallocated carve â†’ NTFS path â†’ format geniÅŸliÄŸi â†’ kurtarma kalitesi â†’ diÄŸer FS â†’ test harness.

---

## BaÅŸarÄ± Metrikleri (ticari kÄ±yas)

| Metrik | BugÃ¼n (tahmini) | Hedef (Faz 0-5 sonrasÄ±) |
|--------|-----------------|-------------------------|
| NTFS silinmiÅŸ dosya bulma (HDD, yeni silme) | ~70% | â‰¥90% |
| NTFS tam yol ile | ~40% | â‰¥75% |
| Carve format sayÄ±sÄ± | 114 | â‰¥250 |
| Deep scan sÃ¼re (500GB HDD) | baseline | â‰¤0.5Ã— (paralel + unallocated) |
| Kurtarma doÄŸrulama | MD5 only | MD5 + yapÄ±sal validator |
| Partition hedefleme | Yok | Var |
| ReFS | Yok | Temel |

---

## Riskler & BilinÃ§li SÄ±nÄ±rlar

1. **TRIM/SSD:** YazÄ±lÄ±mla aÅŸÄ±lamaz; yalnÄ±z uyarÄ± ve erken silme senaryosu.
2. **Ãœzerine yazma:** HiÃ§bir araÃ§ kurtaramaz.
3. **BitLocker TPM:** DonanÄ±m baÄŸlÄ±; password/FVEK dÄ±ÅŸÄ± destek sÄ±nÄ±rlÄ± kalabilir.
4. **APFS tam:** Apple ekosistemi karmaÅŸÄ±k; %100 R-Studio Mac beklentisi gerÃ§ekÃ§i deÄŸil.
5. **Yasal:** Kurtarma aracÄ±; kullanÄ±cÄ± veri sahipliÄŸi ve izin sorumluluÄŸu UI'da hatÄ±rlatÄ±lmalÄ±.

---

## Ä°lk Sprint Ã–nerisi (onay sonrasÄ±)

1. Task 0.1 â€” partition hedefli tarama
2. Task 0.3 â€” unallocated-only carve
3. Task 0.2 â€” D: harfi eÅŸlemesi
4. Task 1.2 â€” NTFS path rebuild

Bu dÃ¶rt task, kullanÄ±cÄ±nÄ±n "D: diskinde silinmiÅŸ veri bul" senaryosuna en doÄŸrudan etkiyi yapar.
