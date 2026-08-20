# Adversarial review — Wolf Recovery (ürün + özellik envanteri)

- Tarih: 2026-08-20
- Kapsam: `wolf-recovery/` HEAD (`ccdacba`), tam ürün yüzeyi; son 4 AR-fix commit dahil
- Yöntem: Saboteur / New Hire / Security Auditor; 2+ persona = bir seviye terfi
- Runtime: statik kod + önceki yerel doğrulama (`typecheck`, Vitest 19, ctest 157, 1 skip). Electron E2E / `npm run dist` / gerçek disk yok.
- Verdict: **BLOCK**

Soru: “proje çalışır durumda mı, her özelliği ile?” Cevap: birim testler yeşil; `npm run dev` native addon üretir. Mahkeme/adli **tam özellik seti çalışmıyor**. Dünkü AR-001/009 kısmen yamalı; Durdur deadlock ve recover IPC bypass duruyor.

## Özet sayıları

| Seviye | Adet |
|--------|------|
| CRITICAL | 3 |
| WARNING | 10 |
| NOTE | 6 |

## Persona notları

- **Saboteur:** StopScan JS thread `join` + worker `TSFN.BlockingCall` kilitler; resident NTFS kurtarma reddedilir (küçük dosya çoğunluğu); RAID0 I/O throw README ile çelişir.
- **New Hire:** `FileRecord.id` tarama sayacı vs SQLite rowid; README `wolf-recovery/.github/workflows` yolu yanlış (CI repo kökünde); VSS “Faz 2 bitti” sentinel.
- **Security Auditor:** `recoverFile` hâlâ renderer `runs` kabul eder (`scanId`/`id` atlanırsa); `sandbox: false` + admin; batch DB miss renderer kaydını tutar.

---

## Critical Findings

### AR20-001 | StopScan / StopContentSearch TSFN deadlock

- Persona: Saboteur, Security Auditor → **CRITICAL** (terfi)
- Yol: `bridge_scan.cpp` `StopScan` ~228–237; `scan_coordinator.cpp` `stopScan` `join`; `onFileFound` `tsfn.BlockingCall`
- Kanıt: Stop JS thread’de `coordinator.stopScan()` → `scanThread.join()`. Worker her dosyada `BlockingCall` ile JS’e döner. JS join bekler, worker JS bekler. Aynı desen `StopContentSearch` + `startSearch` içindeki `stopSearch()`.
- Zarar: Aktif taramada Durdur = admin süreç asılı. Dünkü AR-009 yalnız `terminate` kapandı; deadlock duruyor.

### AR20-002 | Recover hâlâ renderer `runs` (AR-001 kapanmadı)

- Persona: Security Auditor, Saboteur → **CRITICAL**
- Yol: `bridge_wipe.cpp` `RecoverFile` `scanId_ > 0 && rec.id > 0` değilse `FileRecordFromJs`; `BatchRecoverWorker` DB miss’te renderer kaydını **tutar**
- Kanıt: Preload `recoverFile(drive, fileRecord, dest, scanId?)`. `id <= 0` veya `scanId` yok → DB yok. Batch: `getFileById` fail olursa `runs` JS’den kalır.
- Zarar: DevTools/XSS admin süreçte rastgele sektör aralığını “kurtarıldı + MD5” yazar. Dünkü yama yalnız mutlu yol (scanId+pozitif sqlite id).

### AR20-003 | Resident `$DATA` kurtarılmaz (ürün özelliği kırık)

- Persona: Saboteur, New Hire → **CRITICAL** (terfi: işlev kaybı + yanlış “NTFS kurtarma çalışır” beklentisi)
- Yol: `ntfs_parser.cpp` resident → `dataRuns.clear()` + `mainStreamResident`; `recovery_engine.cpp` boş runs + `source=ntfs_mft` → `"no data runs; resident content not extracted"`
- Kanıt: AR-012 MFT dump’ı kesti. İçerik extract yok. Tipik küçük dosya (çok MFT kaydı) recover fail.
- Zarar: Examiner “NTFS kurtarma var” sanır; `.txt`/kısayol/küçük belge yazılmaz. Sessiz sıfır yerine dürüst fail — yine de özellik çalışmıyor.

---

## Warnings

### AR20-004 | Tek `Engine::diskReader_` hâlâ paylaşımlı

- Persona: Saboteur
- Yol: `wolf_engine.h`; `RecoverWorker` `setRaidBackend`/`openDrive`; `ReadSectors` aynı nesne
- `ioMutex_` ırkı keser; recover RAID backend’i hex oturumunun handle’ını çalar. Hex/RAID/kurtarma sırası belirsiz.

### AR20-005 | `success=true` + `zeroFilled` sayacı yetersiz

- Persona: Saboteur, New Hire
- `incrementRecovered` zero-fill yazımında da artar. Alert sayıyı gösterir; Dashboard “kurtarıldı” sıfır pad’i sayar.

### AR20-006 | Hex `paddedZeros` hâlâ ızgara

- Persona: Saboteur
- `readSectors` `success && bytesRead > 0` ise IPC dizi döner. Kısa/pad okuma sıfır sektörü “okundu” gösterir. Fail-closed yalnız `success=false`.

### AR20-007 | `sandbox: false` + `requireAdministrator`

- Persona: Security Auditor
- Native addon gerekçesi duruyor. Renderer RCE = sistem. `asar: true` dist’te `require('../../native/build/Release/wolf_engine.node')` CI `npm run dist` koşmaz.

### AR20-008 | VSS özellik yok; README/RAID metni güncel değil

- Persona: New Hire
- Quick scan `vss_unbound` sentinel. README RAID “bozuk sektörde sıfır, tarama asla düşmez”; RAID0 I/O `throw`. “Motor diske asla yazmaz” + `FILE_SHARE_WRITE` hâlâ açık.

### AR20-009 | RAID `fail_disk` UI/NAPI yok

- Persona: New Hire, Saboteur
- `VirtualRaid::fail_disk` yalnızca native. Examiner bozuk üyeyi işaretleyemez; RAID6 reconstruct I/O fail yoluna bağlı.

### AR20-010 | APFS / BitLocker decrypt / boş alan wipe / E01>4GiB

- Persona: New Hire
- Etiketli tavanlar. “Her özellik” listesinde çalışır sayılmaz. E01>4GiB artık fail (dünkü AR-003 kapandı).

### AR20-011 | Electron E2E yok

- Persona: Saboteur, New Hire
- ctest fixture; IPC deadlock/recover bypass/asar path koşulmaz. CI Windows native+vitest+typecheck var (`/.github/workflows/build.yml`); `dist` yok.

### AR20-012 | İmaj dest allowlist süreç bellek

- Persona: Security Auditor
- `allowedImageDest` Set. Yeniden başlatınca UI path kalsa bile (readonly pick) yeni pick şart. Aynı oturumda doğru.

---

## Notes

- README ağaç `wolf-recovery/.github/workflows` — gerçek yol repo kökü `.github/workflows/build.yml`.
- `uniqueDestPath` 10000 çakışmada boş string (üzerine yazma yok).
- `FileRecord.id` parser `foundCount++` vs SQLite rowid; ResultsView scanId>0 iken DB sayfası kullanır (iyi).
- FTS 16 MiB skip; regex 128 / 20k satır.
- `will-navigate` deny + `setWindowOpenHandler` deny duruyor.
- HFS offset table, BitLocker `-FVE-FS-`, E01 4GiB refuse, insertFilesBatch rc, scan double-start join: dünkü CRITICAL’ların native kısmı testli.

---

## Özellik envanteri (çalışır mı)

| Özellik | Durum |
|---------|--------|
| Sürücü listesi / admin | Çalışır (admin) |
| Quick/deep NTFS/FAT/ext4 | Fixture’da çalışır |
| HFS+ katalog | Çalışır, 25k tavan |
| APFS | Yalnız keşif |
| VSS | Çalışmaz (sentinel) |
| BitLocker | Tespit; decrypt yok |
| Recover non-resident + sqlite id | Çalışır |
| Recover resident / renderer runs | Fail veya bypass |
| RAID 0/1/5/6/10 kur | Çalışır; fail_disk yok |
| RAW imaj + MD5 | Çalışır |
| E01 | <4GiB çalışır; üstü refuse |
| Hex | Çalışır; pad sıfır riski |
| SMART | Çalışır; KALİBRASYONSUZ |
| Disk wipe | Kapalı |
| Dosya wipe | Dialog ile çalışır |
| Kelime/FTS | Çalışır; tavanlar |
| Rapor/CoC | Metin dürüst |
| Case/NSRL | Çalışır |
| USN timeline | Çalışır |
| `npm run dist` | CI doğrulamaz |

## Summary

Dünkü BLOCK’un bir kısmı (E01 wrap, HFS OOB, sahte BitLocker OEM, batch sqlite rc, scan `terminate`, wipe preload, rapor XSS/CoC yalanı) kapanmış ve ctest 157 yeşil. Ürün “her özellik çalışır” değil: VSS/APFS/resident/decrypt/wipe/E01>4GiB yok veya fail. Merge’i bloklayan iki canlı üretim hatası: **Durdur deadlock** ve **recover renderer `runs` bypass**. Resident extract yazılmadan NTFS kurtarma iddiası da ürün yalanı.
