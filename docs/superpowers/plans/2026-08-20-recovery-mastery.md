# Byteback — Tarama & Kurtarma Ustalığı Planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Byteback'yi ticari veri kurtarma yazılımlarına (R-Studio, Recuva, Disk Drill, PhotoRec) yaklaştırmak; ana odak disk tarama, silinmiş veri bulma genişliği ve kurtarma yetkinliği.

**Architecture:** Mevcut `ScanCoordinator` → FS parser'lar → `CarvingEngine` → `RecoveryEngine` hattını genişlet; tarama kapsamını (partition/volume/unallocated), metadata derinliğini (NTFS path rebuild, ReFS), carving kapsamını (imza + özel parser) ve kurtarma kalitesini (preview, doğrulama, dedup) katman katman yükselt.

**Tech Stack:** C++17 native motor, Electron/React UI, SQLite metadata, GoogleTest + Vitest + Playwright.

**Spec:** Bu plan doğrudan kullanıcı gereksiniminden türetildi; ayrı spec dosyası `docs/superpowers/specs/2026-08-20-recovery-mastery-design.md` onay sonrası yazılacak.

## Global Constraints

- Windows-only native I/O (`\\.\PhysicalDriveN`, `DeviceIoControl`).
- Motor kaynak diske `GENERIC_READ`; yazma yalnız kullanıcı hedef yoluna.
- BitLocker bypass yok; FVEK veya recovery password şart.
- SSD TRIM sonrası kurtarma fiziksel olarak imkansız — UI bunu açık söylemeli.
- GPU/CUDA PFAC ilk fazlarda yok; CPU paralelleştirme öncelikli.
- Her faz sonunda `ctest -C Release` + `npm run test` yeşil kalmalı.

---

## Mevcut Durum Özeti (Ağustos 2026)

### Güçlü yanlar (korunacak)
- NTFS: MFT run walk, silinen kayıt (`status=0`), orphan FILE carve (deep), USN timeline, INDX slack, $LogFile hints, LZNT1 kurtarma.
- FAT/exFAT: zincir yürüyüşü, silinmiş 0xE5, VFAT.
- ext4: extent tree, silinmiş inode/dirent.
- Carving: Aho-Corasick ~114 imza, FOV validator, BGC (sektör adımlı, bütçeli).
- BitLocker FVEK + clear-key recovery password, VSS, RAID 0/1/5/6/10.
- Audit SHA-256 zinciri testli; kötü sektör telemetrisi UI'ya bağlı.

### Kritik eksiklikler (ticari araçlara göre)
| Alan | Byteback bugün | R-Studio / PhotoRec / Recuva |
|------|-----------|------------------------------|
| Tarama hedefi | Tüm fiziksel disk | Partition / volume / unallocated seçimi |
| Sürücü harfi (D:) | Yok | Var |
| Carve kapsamı | Tüm disk sektörleri | Çoğunlukla boş küme / unallocated |
| İmza sayısı | ~114 | 300–400+ |
| NTFS path rebuild | Kısmi (MFT parent walk) | Tam dizin ağacı + $LogFile |
| ReFS | Yok | Var |
| Paralel tarama | Tek thread | Çok çekirdek |
| Önizleme | Yok | Resim/PDF/text |
| Dedup | Yok | MFT+carve birleştirme |
| Office/SQLite/Video | Yok | Özel parser |
| E2E kurtarma testi | Duman testi | Senaryo testleri |

---

## Faz 0 — Tarama Altyapısı & UX Temeli (P0)

**Amaç:** Kullanıcı D: gibi bir birimi doğru hedefleyebilsin; carve gereksiz yere tüm diski taramasın.

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
      int64_t partitionStartSector = -1; // -1 = tüm disk
      uint64_t partitionSizeSectors = 0;
      bool carveUnallocatedOnly = false;
  };
  void ScanCoordinator::startScan(const ScanTarget& target, const std::string& scanType, ...);
  ```

- [ ] **Step 1:** `ScanTarget` struct + `DiskReader::setReadBounds(startSector, sectorCount)` ekle (okuma clamp).
- [ ] **Step 2:** `runQuickScan` partition offset/size ile sınırla; carve aynı bounds içinde kalsın.
- [ ] **Step 3:** NAPI `startScan(driveIndex, scanType, opts?)` — `partitionIndex` veya `startSector`+`sizeSectors`.
- [ ] **Step 4:** UI: `listPartitions` API'sini DriveCard'da kullan; partition seçici dropdown.
- [ ] **Step 5:** Test: sentetik GPT görüntüsünde yalnızca 2. partition taranır.

### Task 0.2: Sürücü harfi → fiziksel disk eşlemesi

**Files:**
- Create: `native/src/io/volume_mapper_win.cpp`, `native/include/io/volume_mapper_win.h`
- Modify: `native/src/bridge/bridge_drives.cpp`

**Interfaces:**
- Produces: `std::optional<ScanTarget> resolveDriveLetter(const std::wstring& letter); // e.g. L"D:"`

- [ ] **Step 1:** `QueryDosDeviceW` + `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` ile harf → PhysicalDrive + offset.
- [ ] **Step 2:** NAPI `resolveVolume("D:")` → `{ driveIndex, startSector, sizeSectors, fsType }`.
- [ ] **Step 3:** UI: "D: tara" hızlı kısayolu Dashboard'da.
- [ ] **Step 4:** Test: mock IOCTL veya entegrasyon (test ortamında mevcut birim varsa).

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

- [ ] **Step 1:** NTFS: `$Bitmap` okuyarak free cluster → sektör aralıkları.
- [ ] **Step 2:** FAT: FAT tablosundan free cluster zinciri dışı aralıklar.
- [ ] **Step 3:** `CarvingEngine::scanRanges(reader, ranges, ...)` — mevcut `scan`'i refactor.
- [ ] **Step 4:** Deep scan varsayılan: metadata full partition + carve unallocated only.
- [ ] **Step 5:** Test: NTFS görüntüsünde allocated bölgede carve adayı üretilmez.

### Task 0.4: Tarama profilleri & dürüst etiketler

**Files:**
- Modify: `src/renderer/components/ScanView/ScanView.tsx`, `DriveCard.tsx`

- [ ] **Step 1:** Üç mod: **Hızlı** (metadata, orphan kapalı), **Derin** (metadata + unallocated carve), **Tam Disk Carve** (eski davranış, uyarı ile).
- [ ] **Step 2:** SSD tespit edilince scan başlamadan TRIM uyarısı modal.
- [ ] **Step 3:** Playwright: mod seçimi + uyarı metni smoke.

**Faz 0 çıkış kriteri:** D: harfi veya partition seçilerek deep scan; carve yalnız boş alan; testler yeşil.

---

## Faz 1 — NTFS Metadata Derinliği (P0)

**Amaç:** Silinmiş dosyalarda isim + klasör yolu oranını R-Studio'ya yaklaştır.

### Task 1.1: Tam $MFT kapsamı

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`

- [ ] **Step 1:** Boot `$MFT` run yürüyüşüne ek olarak `$MFT::$DATA` attribute runs ile tüm MFT extent'leri tara.
- [ ] **Step 2:** MFT fragmentasyonu (non-contiguous $MFT) sentetik test görüntüsü.
- [ ] **Step 3:** Orphan carve ile çakışan kayıtları MFT record numarasına göre birleştir.

### Task 1.2: Dizin ağacı yeniden kurulumu

**Files:**
- Create: `native/src/fs/ntfs_path_rebuild.cpp`, `native/include/fs/ntfs_path_rebuild.h`
- Modify: `native/src/fs/ntfs_parser.cpp`

**Interfaces:**
- Produces: `std::string rebuildPath(uint64_t mftRecord, const MftIndex& idx);`

- [ ] **Step 1:** Tüm FILE record'lardan parent MFT ref + isim topla.
- [ ] **Step 2:** Silinmiş parent'lar için `$LogFile` + INDX slack hint'lerini düşük güven skoruyla bağla.
- [ ] **Step 3:** `fr.path` ve `fr.name` alanlarını tam yol olarak doldur (`/Users/foo/bar.jpg`).
- [ ] **Step 4:** Test: silinmiş alt dizin + dosya senaryosu.

### Task 1.3: $LogFile → kurtarılabilir kayıt yükseltme

**Files:**
- Modify: `native/src/fs/ntfs_logfile.cpp`, `ntfs_parser.cpp`

- [ ] **Step 1:** LogFile'dan çıkan isimleri MFT record ID ile eşleştir (LSN/transaction hint).
- [ ] **Step 2:** Eşleşen kayıtların `confidence` alanını yükselt; UI'da "LogFile doğrulandı" etiketi.
- [ ] **Step 3:** Test: `test_ntfs_logfile.cpp` genişlet.

### Task 1.4: USN → silme olayı filtreleme

**Files:**
- Modify: `native/src/fs/ntfs_parser.cpp`, `src/renderer/components/ResultsView/ResultsView.tsx`

- [ ] **Step 1:** USN kayıtlarını timeline'da tut; ayrı "kurtarılabilir" listesine karıştırma.
- [ ] **Step 2:** ResultsView'da "Yalnız silinmiş" filtresi (`status === 0`).
- [ ] **Step 3:** Test: Vitest filtre birimi.

**Faz 1 çıkış kriteri:** NTFS silinmiş dosyalarda %80+ tam yol (sentetik golden image setinde).

---

## Faz 2 — Carving Genişliği & Hız (P1)

**Amaç:** PhotoRec seviyesine yaklaşan format kapsamı ve kabul edilebilir hız.

### Task 2.1: İmza kütüphanesi genişletme

**Files:**
- Modify: `native/src/carver/signature_engine.cpp` (embedded table)
- Create: `resources/signatures-extended.json`
- Create: `native/tests/test_signature_coverage.cpp`

- [ ] **Step 1:** PhotoRec / file(1) magic listesinden 200+ yeni imza ekle (öncelik: jpg, png, mp4, mov, docx, xlsx, pptx, zip, rar, 7z, pdf, sqlite, pst, eml, wav, mp3, heic, cr2, nef, orf, arw).
- [ ] **Step 2:** Her imza için FOV validator veya footer zorunluluğu belirle.
- [ ] **Step 3:** Test: bilinen magic → doğru uzantı.

### Task 2.2: Özel yapısal parser'lar

**Files:**
- Create: `native/src/carver/parsers/zip_family.cpp`, `sqlite_carver.cpp`, `mp4_carver.cpp`
- Modify: `native/src/carver/signature_engine.cpp`

- [ ] **Step 1:** ZIP tabanlı Office (docx/xlsx/pptx): central directory scan.
- [ ] **Step 2:** SQLite: header + schema page validation, boyut tahmini.
- [ ] **Step 3:** MP4/MOV: `ftyp` + `moov`/`mdat` atom walk; moov sonda ise tail search.
- [ ] **Step 4:** Test: her parser için minimal binary fixture.

### Task 2.3: CPU paralel carve

**Files:**
- Modify: `native/src/carver/signature_engine.cpp`
- Create: `native/include/util/thread_pool.h` (minimal, ponytail: std::async veya fixed pool)

- [ ] **Step 1:** Chunk listesini N worker'a böl; sonuçları mutex'li birleştir.
- [ ] **Step 2:** `std::atomic` progress birleştirme.
- [ ] **Step 3:** Benchmark test: 1 GB sentetik imaj, 4 thread vs 1 thread.

### Task 2.4: MFT ↔ carve deduplikasyon

**Files:**
- Create: `native/src/scan/dedup_index.cpp`
- Modify: `native/src/bridge/bridge_scan.cpp`, `native/src/db/metadata_store.cpp`

- [ ] **Step 1:** `(startSector, size, hash prefix)` ile carve/MFT çakışmasını tespit.
- [ ] **Step 2:** Yüksek güvenli MFT kaydını tut; carve kopyasını `duplicate_of` ile işaretle.
- [ ] **Step 3:** UI: "Tekrarları gizle" toggle.

**Faz 2 çıkış kriteri:** 250+ imza; 4 çekirdekte ≥2× hız; Office/SQLite/MP4 carve çalışır.

---

## Faz 3 — Diğer Dosya Sistemleri (P1)

### Task 3.1: ReFS parser (Windows Server / Win10+)

**Files:**
- Create: `native/src/fs/refs_parser.cpp`, `native/include/fs/refs_parser.h`
- Modify: `native/src/fs/partition_scanner.cpp`, `scan_coordinator.cpp`

- [ ] **Step 1:** ReFS süperblok + container tablosu tanıma.
- [ ] **Step 2:** Directory B+ tree walk, silinmiş entry.
- [ ] **Step 3:** Test: ReFS test görüntüsü (veya minimal fixture).

### Task 3.2: exFAT silinmiş entry iyileştirme

**Files:**
- Modify: `native/src/fs/fat_parser.cpp`

- [ ] **Step 1:** Entry-set fragmentation (non-contiguous name entries) toleransı.
- [ ] **Step 2:** Silinmiş + allocated karışık dizin taraması.

### Task 3.3: APFS katalog derinliği

**Files:**
- Modify: `native/src/fs/apfs_container.cpp`, `apfs_parser.cpp`

- [ ] **Step 1:** Snapshot tree walk (dokümante sınır kaldırma).
- [ ] **Step 2:** `apfs_file` keşif → `runs` doldurma oranını artır.
- [ ] **Step 3:** Test: mevcut APFS fixture genişlet.

**Faz 3 çıkış kriteri:** ReFS volume tanınır ve dosya listeler; APFS kurtarılabilir dosya oranı artar.

---

## Faz 4 — Kurtarma Kalitesi & Doğrulama (P1)

### Task 4.1: Kurtarma önizleme

**Files:**
- Create: `native/src/recovery/preview_reader.cpp`
- Modify: `src/renderer/components/ResultsView/ResultsView.tsx`
- Modify: `src/main/ipc-handlers.ts`

- [ ] **Step 1:** İlk 64 KB okuma NAPI `readFilePreview(scanId, fileId)`.
- [ ] **Step 2:** UI: resim thumbnail (jpg/png/gif), text hex+ascii, PDF ilk sayfa (basit).
- [ ] **Step 3:** Test: Vitest preview API mock.

### Task 4.2: Kurtarma sonrası doğrulama

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`

- [ ] **Step 1:** Kurtarılan dosya için validator tekrar çalıştır (carve kaynaklı).
- [ ] **Step 2:** `RecoveryResult.validationScore` + `validationError`.
- [ ] **Step 3:** UI: "Bozuk / Tam" sütunu.

### Task 4.3: Parçalı dosya birleştirme (çoklu run)

**Files:**
- Modify: `native/src/recovery/recovery_engine.cpp`, `bgc.cpp`

- [ ] **Step 1:** BGC `frag1 + gap + frag2` çıktısını `runs` vektörüne yaz (mevcut kısmen var — tamamla).
- [ ] **Step 2:** 3+ parça için sınırlı brute (ponytail: max 3 gap, bütçe).
- [ ] **Step 3:** Test: iki parçalı JPEG golden.

### Task 4.4: BitLocker genişletme (şifre koruyucu)

**Files:**
- Modify: `native/src/fs/bitlocker_unlock.cpp`

- [ ] **Step 1:** Password protector (VMK decrypt) — libmsbde veya minimal PBKDF2 implementasyonu.
- [ ] **Step 2:** TPM/startup-key için açık hata mesajı koru.
- [ ] **Step 3:** Test: bilinen test vector (Microsoft dokümantasyonu).

**Faz 4 çıkış kriteri:** Kullanıcı kurtarmadan önce önizler; bozuk dosya oranı raporlanır.

---

## Faz 5 — Güvenilirlik, Test & Benchmark (P2)

### Task 5.1: Golden image regression suite

**Files:**
- Create: `native/tests/fixtures/ntfs_deleted/`, `native/tests/test_recovery_golden.cpp`
- Create: `scripts/generate-fixtures.ps1`

- [ ] **Step 1:** NTFS: 10 silinmiş dosya (txt, jpg, docx, parçalı jpg).
- [ ] **Step 2:** FAT32: 5 silinmiş dosya.
- [ ] **Step 3:** CI: `ctest -R GoldenRecovery` — bulunan / kurtarılan oran raporu.

### Task 5.2: E2E kurtarma akışı

**Files:**
- Modify: `e2e/electron-smoke.spec.ts`
- Create: `e2e/recovery-flow.spec.ts`

- [ ] **Step 1:** Mock native veya test disk ile scan → result → recover.
- [ ] **Step 2:** MD5 doğrulama.

### Task 5.3: Tarama checkpoint / resume

**Files:**
- Modify: `native/src/scan_coordinator.cpp`, `metadata_store.cpp`

- [ ] **Step 1:** Son taranan sektör SQLite'a yaz.
- [ ] **Step 2:** Kesilen taramayı "Devam et" ile sürdür.
- [ ] **Step 3:** Test: yarıda kes + resume.

### Task 5.4: Performans profili

**Files:**
- Create: `native/tests/bench_scan.cpp` (gtest benchmark veya basit cronometre)

- [ ] **Step 1:** 500 GB simüle (sparse file) deep scan süre raporu.
- [ ] **Step 2:** README'ye honest benchmark tablosu.

**Faz 5 çıkış kriteri:** Golden testler CI'da; E2E recovery pass; resume çalışır.

---

## Faz 6 — İleri (P3, isteğe bağlı)

| Task | Açıklama | Not |
|------|----------|-----|
| 6.1 GPU PFAC | CUDA/OpenCL Aho-Corasick | Büyük yatırım; Faz 2 paralel CPU yetmezse |
| 6.2 ReFS integrity stream | ReFS checksum doğrulama | Server senaryosu |
| 6.3 Cloud / ağ imaj | E01 over network | Adli senaryo |
| 6.4 macOS / Linux I/O | `disk_reader_posix.cpp` | Platform genişleme |
| 6.5 AI dosya sınıflandırma | Entropi + ML kategori | Pazarlama değil, gerçek değer şüpheli |

---

## Öncelik Sırası (Uygulama Sırası)

```
Faz 0 (P0) → Faz 1 (P0) → Faz 2 (P1) → Faz 4.1-4.3 (P1) → Faz 3 (P1) → Faz 4.4 (P2) → Faz 5 (P2) → Faz 6 (P3)
```

**Gerekçe:** Önce doğru hedef (D:/partition) + unallocated carve → NTFS path → format genişliği → kurtarma kalitesi → diğer FS → test harness.

---

## Başarı Metrikleri (ticari kıyas)

| Metrik | Bugün (tahmini) | Hedef (Faz 0-5 sonrası) |
|--------|-----------------|-------------------------|
| NTFS silinmiş dosya bulma (HDD, yeni silme) | ~70% | ≥90% |
| NTFS tam yol ile | ~40% | ≥75% |
| Carve format sayısı | 114 | ≥250 |
| Deep scan süre (500GB HDD) | baseline | ≤0.5× (paralel + unallocated) |
| Kurtarma doğrulama | MD5 only | MD5 + yapısal validator |
| Partition hedefleme | Yok | Var |
| ReFS | Yok | Temel |

---

## Riskler & Bilinçli Sınırlar

1. **TRIM/SSD:** Yazılımla aşılamaz; yalnız uyarı ve erken silme senaryosu.
2. **Üzerine yazma:** Hiçbir araç kurtaramaz.
3. **BitLocker TPM:** Donanım bağlı; password/FVEK dışı destek sınırlı kalabilir.
4. **APFS tam:** Apple ekosistemi karmaşık; %100 R-Studio Mac beklentisi gerçekçi değil.
5. **Yasal:** Kurtarma aracı; kullanıcı veri sahipliği ve izin sorumluluğu UI'da hatırlatılmalı.

---

## İlk Sprint Önerisi (onay sonrası)

1. Task 0.1 — partition hedefli tarama
2. Task 0.3 — unallocated-only carve
3. Task 0.2 — D: harfi eşlemesi
4. Task 1.2 — NTFS path rebuild

Bu dört task, kullanıcının "D: diskinde silinmiş veri bul" senaryosuna en doğrudan etkiyi yapar.
