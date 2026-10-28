# Byteback mühendislik denetimi

Tarih: 2026-10-28. Kapsam: mevcut çalışma ağacı (`0.1.0`). Yöntem: salt okunur kod okuma, `tsc` (node), `npm audit`, git pickaxe. Kaynak dosyalara ve `.gitignore` dosyasına yazılmadı. Ham notlar: `audit/NOTES.md`.

Bu rapor Disk Drill veya R-Studio paritesi iddia etmez. Saha USB A/B/C bu oturumda çalıştırılmadı.

**Güncel durum (kod):** `audit/REMEDIATION.md`, `audit/OBJECTIVE_CHECKLIST.md`. Aşağıdaki bulgular denetim anındaki kanıttır; Faz 1–3 mühendislik maddeleri giderildi, Faz 4 operatör kapısı açık.

---

## Bölüm 1: Proje Bağlamı, Tehdit Modeli ve Mühendislik Karnesi

**Bağlam.** Byteback, Windows üzerinde yönetici olarak çalışan bir masaüstü veri kurtarma / inceleme uygulamasıdır. Electron ana süreç, sandbox’lı React arayüzü ve süreç içi C++ motor (`byteback_engine.node`) kullanır. Ağ servisi, hesap sistemi ve çok kiracılı API yoktur.

**Kritik varlıklar.** Kanıt diski ve imajı, kurtarılan dosyalar, `userData/byteback.db` (WAL), hash zincirli audit log, BitLocker/LUKS anahtar malzemesi (süreç belleği), imha hedefi.

**Dış yüzey.** Yerel IPC (preload allowlist), dosya diyalogları, `\\.\PhysicalDrive` okuma, açık imha yazması, isteğe bağlı HTTP Range kodu (ürün dosya seçicisi http’yi kabul etmez).

**Tehdit modeli.** Saldırgan, operatörün açtığı kötü niyetli bir imaj veya regex ile admin süreçte bellek/CPU hatası arar. İkinci senaryo, onayları geçmiş bir imhanın yanlış diske yazılmasıdır. Uzak web saldırganı bu sürümün birincil tehdidi değildir.

**Puanlar (100).** Rubrik: 90–100 üretime hazır; 75–89 çalışır, orta borç; 50–74 ciddi boşluk; 0–49 çıkmamalı.

| Kriter | Puan | Gerekçe |
|---|---|---|
| Mimari ve kod kalitesi | 78 | Katmanlar ve `contextIsolation` duruyor. `ipc-handlers.ts` ve `vhd_source.cpp` şişmiş. |
| İş mantığı ve kararlılık | 74 | Native/Vitest yüzeyi geniş. Ad regex’i guard’sız. WAL `synchronous=NORMAL`. HTTP özelliği dokümanda var, üründe kapalı. |
| Güvenlik ve tedarik zinciri | 76 | Kurtarma yolu, imha serisi, üretim `npm audit` 0. Düşüren: imzasız admin süreçte güvenilmeyen imaj ayrıştırması; dev ağacında xmldom/vitest advisory. |
| Performans ve kaynak | 80 | MFT akışı, hex 64 KiB sınırı, sanal sonuç satırları. VHD ailesi 64 MiB RAM tavanı bilinçli ve hatalı büyük imajı reddeder. |
| UX | 72 | İnceleme akışı, dürüstlük bantları, diyaloglar var. React error boundary yok. Saha USB doğrulanmadı. |
| Test ve üretime hazırlık | 64 | CI’da typecheck, Windows native test, Vitest, Playwright adımı var. POSIX sözdizimi listesi yeni ayrıştırıcıları kapsamıyor. Sürüm `0.1.0`. İmza ve saha A/B/C açık. |

Canlı “1.0 forensic ürün” olarak çıkmamalı. Laboratuvar inceleme aracı olarak çalışır durumda; orta seviye borç ve bilinçli tavanlar var.

---

## Bölüm 2: Bozuk Akışlar, İş Mantığı Hataları ve Eşzamanlılık

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `src/main/ipc-handlers.ts:796-809` ve `native/src/db/metadata_store.cpp:1514-1516` (içerik aramasında guard var, ad aramasında yok)

```ts
if (useRegex && typeof query === 'string' && query.length > 128) {
  return { rows: [], error: 'Regex sorgusu en fazla 128 karakter olabilir.' }
}
```

```cpp
std::regex re;
re = std::regex(query, std::regex::icase | std::regex::ECMAScript);
```

* **Sorun & Tetiklenme Yolu:** Anahtar kelime araması regex açıkken `searchFiles` çağırır. IPC yalnız uzunluğu keser. `contentRegexQueryOk` (`native/src/search/content_search.cpp`, içerik araması `bridge_scan.cpp` içinde) bu yola bağlı değildir. `(a+)+$` gibi 128 karakter altı bir kalıp, MSVC `std::regex` ile 20.000 satır taramasında ana süreci kilitler. Arayüz vurgusu `isSafeHighlightRegex` ile kendini korur; motor korumaz.
* **Etki:** İnceleme oturumu donar. Veri diske yazılmaz. Yerel kendini hizmet dışı bırakmadır, uzak RCE değildir.
* **Mühendislik Çözümü:** `SearchFiles` içinde `contentRegexQueryOk` başarısızsa boş sonuç ve `regex rejected` dön. IPC ile motor aynı kuralı kullansın.

**[Önem: ORTA]** — **[Güven: Muhtemel]** — `native/src/db/metadata_store.cpp:318-319`

```cpp
sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
```

* **Sorun & Tetiklenme Yolu:** WAL + `NORMAL`, işletim sistemi çökmesi veya güç kesintisinde son commit’in diske inmemiş olabileceği SQLite modudur. Tarama checkpoint’i bu pencerede kesilirse devam kaydı eksik kalabilir.
* **Etki:** Dava veritabanında son yazılan dosya satırları veya tarama durumu kaybolabilir. Kanıt diski değişmez.
* **Mühendislik Çözümü:** Dava DB için `synchronous=FULL` (veya checkpoint sonrası `wal_checkpoint(FULL)`). Hızı ölç; 2M MFT yazımında farkı ayrı test et.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `src/main/ipc-handlers.ts:1006-1039`

```ts
if (confirmPhrase !== 'IMHA') {
  return { ok: false, error: 'Onay ifadesi IMHA olmalı' }
}
```

* **Sorun & Tetiklenme Yolu:** Seri eşleşmesi, `IMHA` ve OS diyalogu var. Diyalog açıkken başka bir tarama aynı indekse başlarsa `lastEvidenceDisks` kontrolü diyalogdan önce yapılmıştır. İndeks, diyalog süresince yeniden bağlanmış bir diske kayabilir (USB çıkar-tak).
* **Etki:** Operatör hâlâ seri numarasını yazmak zorunda. Native `wipeSerialMatches` yazmadan önce seriyi tekrar okur (`data_shredder.cpp`). Pratik risk, seri aynı kalan sıcak takas değil; kontrol sırasının tek anlık görüntü olması.
* **Mühendislik Çözümü:** Yazmadan hemen önce seri ve “tarama/imaj canlı” kontrolünü native girişinde bir kez daha yap. Mevcut seri kapısı kalsın.

### Düşük öncelik özeti

| Konu | Güven | Not |
|---|---|---|
| PDF raporu SHA-256 hex’i HTML’e gömülü | Doğrulandı | Hex; kaçış şart değil |
| `export-report-pdf` geçici HTML | Doğrulandı | `finally` içinde siliniyor |
| Tek örnek kilidi | Doğrulandı | İkinci süreç çıkar |

---

## Bölüm 3: Yarım Kalan Özellikler, Kopuk Uçlar ve Ölü Kodlar

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `README.md:53-54` ve `native/include/byteback_io.h:269-274`

```text
- **E01 read** — local and HTTP Range multi-segment `.E01`→`.E02` via
  `attachEwfImage` / `attachHttpRawImage` / `attachRawFile` on `DiskReader`.
```

```cpp
if (path.empty() || isHttpUrl(path)) return fail("invalid image path");
```

* **Sorun & Tetiklenme Yolu:** Dosya seçici ve `isEvidenceImagePath` `http://` reddeder. `startScan` NAPI http imaj yolunu düşürür. `EwfReader` http dalı ve `openHttpByteSource` kodda durur; inceleme arayüzünden ulaşılamaz.
* **Etki:** Operatör uzak E01 beklerse özellik yoktur. Doküman tersini söyler. Allowlist açılırsa aşağıdaki host süzgeci yetersiz kalır (Bölüm 4’te tekrarlanmadı; aynı özelliğin ön koşulu).
* **Mühendislik Çözümü:** Ya README’den HTTP cümlesini sil ya da ürün yolunu açmadan önce host’u WinHTTP’nin çözdüğü adresle yeniden denetle. `fc`/`fd` önekini yalnız IPv6 literallerine uygula (`byte_source.cpp:464`).

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `native/src/io/byte_source.cpp:464`

```cpp
if (h.rfind("fe80:", 0) == 0 || h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true;
```

* **Sorun & Tetiklenme Yolu:** Köşeli parantez dışı her ad. `fdic.gov` veya `forecast.example.com` özel adres sayılır. DNS ile `127.0.0.1`’e giden halka açık bir ad engellenmez. `http://example.com@127.0.0.1/` host dizesi dört oktet sayılmaz.
* **Etki:** Bugün ürün yolu kapalı olduğu için operatör tetikleyemez. Özellik bağlanırsa yanlış red ve SSRF birlikte gelir.
* **Mühendislik Çözümü:** Engeli `WinHttpCrackUrl` host’una ve bağlanılan IP’ye taşı. Kullanıcı bilgisi (`user@host`) içeren URL’yi reddet.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `docs/release-checklist.md` ve kod tavanları

* **Sorun:** Storage Spaces slab birleştirme, LUKS2/Argon2, UDIF LZFSE/bzip2, Btrfs/ZFS, Mac native, ağ ajanı, WinPE, BitLocker TPM bilinçli olarak yok. Checklist bunu “v1 dışı” diye ayırır. Eksik özellik değil, kapsam sınırı.
* **Etki:** Rakip broşürüyle kıyaslayan kullanıcı bunları bug sanır.
* **Çözüm:** Sınırı UI’da ilgili akışta tek cümle göster (ör. Spaces yalnız keşif). Kod tekrarı yazma.

### Düşük öncelik özeti

| Konu | Not |
|---|---|
| `seed-scan-fixture` preload’da | Main `BYTEBACK_E2E===1` kapısı var |
| Sektör başına audit log yok | Yorumda bilinçli; tarama gürültüsü |

---

## Bölüm 4: Uygulama Güvenliği, Tedarik Zinciri ve Veri Gizliliği

**[Önem: YÜKSEK]** — **[Güven: Şüpheli]** — `package.json:71` ve `src/main/main.ts:94-98`

```json
"requestedExecutionLevel": "requireAdministrator"
```

```ts
contextIsolation: true,
nodeIntegration: false,
sandbox: true
```

* **Sorun & Tetiklenme Yolu:** Arayüz sandbox’lı. Ayrıştırıcılar (NTFS, VHD ailesi, E01, ISO, UDF, LUKS) aynı yönetici sürecinde, güvenilmeyen baytlar üzerinde çalışır. Bu denetim belirli bir bellek bozulması kanıtlamadı. Tetik, operatörün crafted imaj açmasıdır.
* **Etki:** Ayrıştırıcıda bir bellek hatası olursa kod yönetici olarak çalışır. Kurtarma dosyası yazma ve imha aynı süreçtedir.
* **Mühendislik Çözümü:** İmaj ayrıştırmayı düşük yetkili bir alt sürece al; diske ham yazmayı ayrı, onaylı bir yardım sürecinde tut. Kısa vadede hostile imaj fuzz’ı (mevcut gtest sentetiklerinin üstüne) bu riskin gerçek olup olmadığını ölçer.

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `npm audit` 2026-10-28; `package-lock.json` `@xmldom/xmldom@0.9.11` `"dev": true`

* **Sorun & Tetiklenme Yolu:** `npm audit --omit=dev` 0 bulgu. Tam ağaç: Vitest ≤4.1.10 moderate (GHSA-82fw-gwwq-j7x9, GHSA-qxg6-89mg-rqv8); xmldom 0.9.11 high (GHSA-6gmq-8vp8-gcm6 ve aynı paket üzerindeki kardeş GHSA’lar). İkisi de production `dependencies` içinde değil. Çalışan NSIS uygulamasına girmez. Paketleme makinesinde electron-builder XML işlerse xmldom devreye girebilir.
* **Etki:** Müşteri diskine bu advisory’ler üzerinden uzaktan erişim kanıtlanmadı. CI makinesi kötü XML görürse derleme zamanı riski vardır.
* **Mühendislik Çözümü:** Üretim ağacını `npm audit --omit=dev` ile kapıda tut. Dev advisory’yi electron-builder / vitest yükseltmesiyle kapat; `--force` ile kör yükseltme yapma.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — git pickaxe

* **Sorun:** Son taranan geçmişte `AKIA` ve `BEGIN PRIVATE` yok. `fvek` geçen commit’ler özellik ekler, anahtar gömmez. `.env` yok.
* **Etki:** Bu aramada sızmış bulut anahtarı yok.
* **Çözüm:** CI’ya gitleaks ekle. Bu turda araç kurulu değildi.

Kurtarma hedefi main süreçte `validateRecoverDestDir` ile elenir (`ipc-handlers.ts` recover çağrıları). Fiziksel imha seri eşleşmesi olmadan `shred_physical_drive` içinde dönmez. SQL arama parametre bağlar; FTS kalıbı tırnakla kaçırılır (`buildFtsMatch`). Bunlar bulgu değil, duran kontrollerdir.

Kişisel veri: dava alanları ve dosya adları yerel SQLite ve PDF’te düz metindir. Uzak log yok. `crashReporter` `uploadToServer: false`. Oturum logu yol redaksiyonu `redact-paths` ile durur. KVKK kapsamı, operatörün kendi makinesindeki dava kopyasıdır; ayrı bir sunucu envanteri yoktur.

### Düşük öncelik özeti

| Konu | Güven | Not |
|---|---|---|
| BitLocker/LUKS parola süreç belleğinde | Doğrulandı | Beklenen; diske loglanmıyor |
| GitHub Action `@v4` | Doğrulandı | Major pin; tam SHA değil |
| SQLite 3.53.4 (2026-07-24) | Doğrulandı | Bu turda CVE eşlemesi yapılmadı |

---

## Bölüm 5: Yazılım Mimarisi, Performans ve Kaynak Yönetimi

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `src/main/ipc-handlers.ts` (dosya ~1490 satır), `native/src/io/vhd_source.cpp` (~2000 satır), `native/src/fs/ntfs_parser.cpp`, `native/src/fs/raid_detect.cpp`

* **Sorun & Tetiklenme Yolu:** IPC, tarama, imha, RAID, LUKS ve rapor aynı modülde. Sanal disk ailesi tek çeviri biriminde. Yeni ayrıştırıcı eklemek bu dosyalarda yan etki aramasını büyütür.
* **Etki:** İnceleme hatası yanlış alt sisteme kayar. Derleme birimleri (özellikle `vhd_source.cpp` ve `byteback_io.h`) geniş yeniden derleme üretir.
* **Mühendislik Çözümü:** IPC’yi mevcut `bridge_*.cpp` sınırlarıyla hizala (scan / image / wipe / raid). VHD, VHDX, VMDK, VDI, QCOW2, DMG ayrı çeviri birimleri olsun. Davranış testi eklemeden bölme.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `native/src/io/vhd_source.cpp:27` ve boyut kontrolleri (`virt > kMaxExpand` reddi)

```cpp
constexpr uint64_t kMaxExpand = 64ull << 20;
```

* **Sorun:** Sanal disk yükü tüm sanal boyutu RAM’e açar, 64 MiB üstünü hata ile keser. Sessiz kesme yok.
* **Etki:** Büyük VHD/VMDK incelemesi bu sürümde yoktur. Bellek sınırı tutulur.
* **Çözüm:** Ürün metninde kalabilir. Akışlı okuma ayrı iş; bu turda borç olarak yaz, bug olarak yazma.

`quick_check` tam `integrity_check` değildir; kod yorumu bunu söyler. Büyük dava DB’sinde açılış süresini kısa tutar, indeks çapraz kontrolünü atlar.

### Düşük öncelik özeti

| Konu | Not |
|---|---|
| Hex okuma 1–65536 bayt | IPC’de sınırlı |
| Thumb protokolü dizin dışına 403 | `resolveThumbUrlToPath` |
| Tek SQLite bağlantısı + recursive mutex | Bilinçli tek süreç |

---

## Bölüm 6: Arayüz, Kullanılabilirlik ve Durum Yönetimi

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `src/renderer/App.tsx` (ErrorBoundary yok; depo taraması eşleşmedi)

* **Sorun & Tetiklenme Yolu:** Bir sonuç veya hex bileşeni render sırasında fırlarsa React ağacı düşer. Ana süreç `render-process-gone` yazar; arayüzde sınırlı bir hata paneli yoktur.
* **Etki:** Operatör beyaz/çökmüş pencere görür. Tarama native tarafta sürebilir; pencere kapanış uyarısı `isScanLive` ile durur, renderer ölüyse bu uyarı da ölür.
* **Mühendislik Çözümü:** Sayfa köküne bir error boundary. Metin: tarama durmadıysa “yeniden yükle”, durduysa son scan id. Ayrıntıyı oturum loguna yaz, müşteriye stack basma.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `src/main/main.ts:90-91`

```ts
minWidth: 1024,
minHeight: 680,
```

* **Sorun:** Masaüstü inceleme aracı. 1024 altı yok. RAID ve sonuç tablolarında `aria-*` örnekleri var; tam klavye denetimi bu turda tarayıcıda doğrulanmadı.
* **Etki:** Küçük dizüstü penceresinde sıkışma. Mobil hedef değil.
* **Çözüm:** 1280 referans genişlikte sonuç + hex’i elle gez. Ayrı bir “mobil” işi açma.

Boş tarama, imaj iptali ve imha iptali diyalogları kodda durur. Yükleme iskeleti her listede zorunlu değil; sayfalı sonuç var.

### Düşük öncelik özeti

| Konu | Not |
|---|---|
| Dil TR/EN | i18n anahtar testleri var |
| PDF raporu | Dava alanları `htmlEscape` |

---

## Bölüm 7: Dayanıklılık, Gözlemlenebilirlik, Test ve CI/CD

**[Önem: ORTA]** — **[Güven: Doğrulandı]** — `.github/workflows/build.yml:55-110`

* **Sorun & Tetiklenme Yolu:** Linux `g++ -fsyntax-only` listesi NTFS/FAT/ext4/XFS/APFS içerir. `iso9660_parser.cpp`, `udf_parser.cpp`, `luks.cpp`, `ldm_parser.cpp`, `lvm_parser.cpp`, `vhd_source.cpp`, `hex_search.cpp`, `mft_record_view.cpp` listede yoktur. Windows işi CMake ile derler; POSIX’e özgü sözdizimi hatası o işte görünmez.
* **Etki:** Linux sözdizimi kapısı, CA-056’nın gerekçesini yeni dosyalarda karşılamaz. Bu turda o dosyalarda POSIX derleme hatası kanıtlanmadı.
* **Mühendislik Çözümü:** Listeyi `native/src` altındaki platformdan bağımsız `.cpp` dosyalarından üret. Windows-only dosyaları (`disk_reader_win.cpp`, `data_shredder.cpp`, `smart_monitor.cpp`) dışla.

**[Önem: YÜKSEK]** — **[Güven: Doğrulandı]** — `package.json` `"version": "0.1.0"` ve `docs/release-checklist.md`

* **Sorun:** Authenticode, temiz kutu NSIS, tam Playwright, CHANGELOG, saha A/B/C işaretli değil. Checklist “yeşil CI = 1.0 değil” der.
* **Etki:** İmzası olmayan yönetici kurulum SmartScreen ve adli kabul için zayıf kalır. Saha MD5 iddiası bu raporda yoktur.
* **Mühendislik Çözümü:** 1.0 etiketini checklist kapanmadan basma. İmza sertifikası kullanıcıdadır; kod bunu taklit etmesin.

**[Önem: DÜŞÜK]** — **[Güven: Doğrulandı]** — `src/main/main.ts:124-126`

```ts
mainWindow.webContents.on('render-process-gone', (_event, details) => {
  appendSessionLog('RENDER_GONE', `reason=${details.reason} exit=${details.exitCode}`)
```

* **Sorun:** Yapılandırılmış uzak izleme yok. Yerel oturum logu ve audit zinciri var. Sağlık ucu yok; masaüstü için beklenen.
* **Etki:** Çökme sonrası inceleme yalnız yerel log ile olur.
* **Çözüm:** Uzak telemetry ekleme. Loga parola/FVEK yazılmadığını koru.

Bu turda `ctest` ve Playwright çalıştırılmadı. Windows CI işi bunları tanımlar; son `main` sonucu bu oturumda okunmadı.

### Düşük öncelik özeti

| Konu | Not |
|---|---|
| `tsconfig.node.json` | Bu turda `tsc --noEmit` geçti |
| `tsconfig.web.json` | Bu turda koşulmadı |
| eslint betiği | `package.json` içinde yok |

---

## Bölüm 8: Sektör Kıyaslaması ve Ürün Geliştirme Önerileri

Konum: Windows masaüstü kurtarma. Hedef kitle laboratuvar operatörü. Aşağısı bulgu listesinden ayrıdır.

Kaynaklar (erişim 2026-10-28):

- Disk Drill 6 dosya sistemi matrisi: https://www.cleverfiles.com/help/disk-drill-mac-windows.html
- R-Studio: https://www.r-studio.com/
- R-Studio Technician yetenekleri: https://www.r-studio.com/Data_Recovery_Technician.shtml ve https://www.r-studio.com/Unformat_Help/r-studio-technician.html
- NIST SP 800-88 Rev. 2, Eylül 2025: https://csrc.nist.gov/pubs/sp/800/88/r2/final

Gözlem:

- Disk Drill 6 Windows, NTFS/ReFS/FAT/exFAT/APFS/ext/Btrfs imzası ve 400+ imza, RAID, imaj, hex listeler. Adli mod ve bootable Windows’ta “yok / beta” diye ayrılır. Byteback Btrfs ayrıştırmaz; imza seti 150 civarındadır ve metin sihirlerini bilinçli atar.
- R-Studio NTFS, ReFS, FAT, exFAT, HFS/APFS, XFS, UFS, ext ve ağ diskini listeler. Technician; çoklu geçiş imaj, sanal diski sisteme bağlama, ağ ajanı, BitLocker `.bek`/VMK, adli rapor iddiası taşır. Byteback UFS, ağ ajanı ve VMK/TPM yapmaz. LUKS1 ve mdadm/LVM/LDM laboratuvar kodu rakipten farklı bir yöndür.
- NIST 800-88r2, çok geçişli DoD 5220.22-M üzerine yazmayı güncel sanitizasyon saymaz; SSD’de host overwrite yetersizdir. Byteback imha metni “NIST 800-88 değildir” der. Bu, broşürdeki “DoD 3 geçiş” iddiasından daha doğru bir tavırdır. Ürünleştirmede Clear/Purge ayrımını UI’da bırak.

Ürün işleri (bulgu tamiri değil):

1. Saha A/B/C (NTFS, FAT32 JPEG MD5, exFAT) checklist’teki gibi, uydurma PASS olmadan.
2. İmajın akışlı okunması (64 MiB tavanını kaldırmadan önce ölçüm).
3. Çok geçişli imaj ve sektör haritası (R-Studio Technician’daki runtime imaging sınıfı).
4. Btrfs veya UFS’den birini seçmek; ikisini birden “v1”ye koyma.
5. İmza sayısını 400’e şişirmeden, metin sihiri yasağını koruyarak kamera RAW kapsamını ayrıca yazmak.

---

## Bölüm 9: Fazlandırılmış Mimari Eylem Planı

**Faz 1.** Ad aramasına `contentRegexQueryOk` (Bölüm 2). HTTP cümlesini README’den sil veya özelliği kapalı tutup allowlist’i bağlamadan bırak (Bölüm 3). 1.0 etiketi yok (Bölüm 7).

**Faz 2.** POSIX sözdizimi listesini dosya sisteminden üret (Bölüm 7). Dev `npm audit` (vitest, xmldom) yükseltmesini ayrı PR’da dene (Bölüm 4). `synchronous=FULL` ölçümü (Bölüm 2).

**Faz 3.** IPC ve `vhd_source.cpp` bölünmesi, testler yeşil kalacak şekilde (Bölüm 5). React error boundary (Bölüm 6). İmaj ayrıştırıcı fuzz örneği (Bölüm 4, exploit kanıtı yokken süreç ayrımı ertelenebilir).

**Faz 4.** Authenticode, temiz kutu NSIS, saha A/B/C, CHANGELOG (Bölüm 7). Bölüm 8’deki akışlı imaj ve seçilmiş bir dosya sistemi. Uzak telemetry ekleme.

---

## Bölüm 10: Denetim Kapsam Beyanı

1. **Tam okunan veya uçtan uca izlenen:** `package.json`, `docs/ARCHITECTURE.md`, `docs/release-checklist.md`, `wiki/Security.md`, `.github/workflows/build.yml`, `src/main/main.ts`, `src/preload/index.ts`, `src/main/recover-dest-validator.ts` (baş), `src/main/image-dest-allowlist.ts`, `src/shared/win32-volume-path.ts`, `src/main/ipc-handlers.ts` (seed, hex, imha, arama, imaj seçici), `native/src/io/byte_source.cpp` (HTTP host ve WinHTTP), `native/include/byteback_io.h` (`attachEvidenceImage`), `native/src/bridge/bridge_scan.cpp` (imagePath ve SearchFiles), `native/src/db/metadata_store.cpp` (PRAGMA, FTS, regex), `native/src/search/content_search.cpp` (`regexQueryOk`), `native/src/recovery/path_util.cpp` (`destDirIsSafe`), `src/renderer/components/ReportView/ReportGenerator.tsx` (kaçış), `src/renderer/components/SearchView/KeywordSearch.tsx`, `native/third_party/sqlite3.h` sürüm makrosu, `README.md` E01 paragrafı.

2. **Kısmen veya atlanan:** `native/src/fs/ntfs_parser.cpp`, `raid_detect.cpp`, `vhd_source.cpp` yalnız boyut ve tavan için örneklendi; her dal okunmadı. `native/third_party` gövdesi (zlib/sqlite kaynak) satır satır okunmadı. `e2e/` senaryoları çalıştırılmadı. Önceki `docs/codebase-audit/` raporları tarihsel; bu raporun kanıtı yerine geçmez.

3. **Koşan komutlar:** `npm audit --omit=dev` → 0. `npm audit` → vitest moderate, xmldom 0.9.11 high, ikisi dev. `npx tsc --noEmit -p tsconfig.node.json` → exit 0. `git log -S AKIA`, `git log -S "BEGIN PRIVATE"` → eşleşme yok.

4. **Koşulmayan:** eslint, semgrep, gitleaks, trivy, osv-scanner, bandit, `ctest`, Vitest, Playwright, `tsconfig.web.json`. Kurulu olmayan veya bu turda kasıtlı çalıştırılmayan araçlar bulgu sayılmaz.
