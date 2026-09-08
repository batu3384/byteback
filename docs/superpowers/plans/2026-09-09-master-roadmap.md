# Byteback — Kalan İş Listesi Master Yol Haritası (2026-09-09)

> **For agentic workers:** Bu belge bir **master roadmap**'tir: faz sırası, madde bazlı tasarım eskizi, risk ve kabul kriterleri içerir. Her fazın icrası başlarken bu belge kaynak alınarak `writing-plans` formatında **ayrı görev planı** üretilir (test kodu dahil adım adım). Maddeler `- [ ]` sözdizimiyle izlenir.

**Hedef:** Kalan tüm işleri (motor ölçeği, silinmiş-durup derinliği, format genişletme, inceleyici araçları, üretim sertleştirme, saha doğrulaması) bağımlılık-değer-risk sırasına göre fazlara bölerek profesyonel ürün seviyesine taşımak.

**Mimari zemin:** Electron 44 + React 18 + C++17 N-API; 523 native / 213 vitest / 15 e2e yeşil; NSIS installer doğrulanmış; paralel carve + imaging resume + E01 zlib + audit zinciri tamamlanmış. Motor katmanı olgın; eksik olan **ölçek**, **silinmiş-durup derinliği**, **inceleyici ergonomisi** ve **saha kanıtı**.

**Spec kaynakları:** `docs/competitive-analysis-2026-09-05.md` (12 maddelik yol haritası), `memory/byteback-project.md` (ertelenen tavanlar envanteri), motor değerlendirme raporu (ölçek bulguları).

## Global Constraints

- Tek branch `main`, conventional commit (`type(scope): özet`), her madde kendi test döngüsüyle.
- Kapı her commit'te: `ctest` (523+) + `npx vitest run` (213+) + `npx playwright test` (15+) + `tsc` ×3.
- Yeni dependency yok (zlib gibi istisnalar vendored ve gerekçeli); framework yok.
- Adli dürüstlük: her yeni yetenek ya kanıtla doğrulanır ya dürüstçe "desteklenmiyor" der — asla tahmin üretmez.
- parser offset'leri gerçek spec'ten cite'lenir (kernel header / MS doc / libbde tarzı); self-consistent fixture tek başına yeterli sayılmaz, en az bir golden hex-literal assert şart.
- Windows gate'i `#ifndef _WIN32` yollarını göremez: posix'e değen her değişiklik CI `native-posix-syntax` job'unun kapsamında kalır.

---

## Faz sıralamasının mantığı

```
Faz 0  Saha Doğrulaması    → her şeyin önceliğini gerçek veriyle belirler (hızlı, bloklamaz)
Faz 1  Ölçek (8TB)         → saha testinde çıkacak "büyük disk" bulgularının altyapısı
Faz 2  Silinmiş-durup      → adli değer paritesi (müşteri gözünde en görünür fark)
Faz 3  Format genişletme   → kapsama alanı (hangi diskler açılır)
Faz 4  İnceleyici araçları → uzman ergonomisi (günlük kullanım konforu)
Faz 5  Üretim sertleştirme→ dağıtım güveni (imza, unicode, kararlılık)
```

Faz 0 ile Faz 1 paralel yürüyebilir (0 saha protokolü hazırlarken 1'in tasarımı yazılır). Faz 2-3 arası bağımlılık yoktur; kaynak müsaitse paralel kol çalıştırılır. Faz 4, Faz 2-3'ten bağımsızdır. Faz 5 her zaman en sona bırakılır ama içindeki S-boyutlu maddeler (unicode, HTTP fallback) boş luklarında öne çekilebilir.

---

## FAZ 0 — Saha Doğrulaması ( tahmini 1-2 gün + test süresi)

**Gerekçe:** Projedeki en büyük bilinmeyen hiçbir testin kapsamadığı şey: gerçek donanım. Memory'deki tespit doğru: "gerçek disk doğrulaması — diğer her şey bunu bekliyor". Tüm sentetik fixture'lar motorla aynı zihniyetle yazıldı; gerçek disk, gerçek firmware tuhaflıkları (512e4Kn, USB köprüsü gecikmeleri, SMART tablo varyasyonları) yalnızca sahada görülür.

### 0.1 Test protokolü dokümanı — [ ] 
`docs/field-test-protocol.md` oluştur. İçerik: test matrisi (donanım × senaryo × beklenen × gerçek × kanıt), adım adım prosedür, ekran görüntüsü/log toplama talimatı.
- Matris satırları: (a) NTFS HDD intern — quick/deep/full carve; (b) FAT32 USB 8-32GB — silinmiş foto kurtarma uçtan uca (kurtar → MD5 karşılaştır); (c) exFAT SD kart; (d) kasıtlı bozuk MFT'li imaj (gerçek diskte boot sektörünü bozmadan test imajı yazılı USB); (e) kötü bölgeli USB (hdmdeğirmeni/HDDScan ile işaretlenmiş); (f) E01 imaj alma → yeniden aç → verifyDigest; (g) RAID kurulum varsa değilse 3 USB + yazılım RAID5 imajları üzerinden; (h) BitLocker'lı ikincil bölüm (kurtarma anahtarıyla).
- Her satırın kanıt hedefi: süre, bulunan kayıt sayısı, yanlış-pozitif gözlemi, UI tepkisi, audit zincirin tamamlandığı.
**Doğrulama:** doküman review; protokol bir kez uçtan uca dry-run.
**Boyut:** S. **Risk:** yok.

### 0.2 Saha test paketi yardımcıları — [ ]
Uygulama içinde gerekli gözlemler: (a) tarama özeti ekranında süre+ray sayısı zaten var — eksikse "hedef disk seri no" gösterimi ekle (kanıt bağlamı); (b) `window.confirm` kalıntısı yok — doğrulandı; (c) log toplama: session.log + audit.log + DB `app.getPath('userData')` yolunu Ayarlar/About görünümünde kopyalanabilir göster (yoksa ekle).
**Dosyalar:** `src/renderer/components/CaseView/CaseView.tsx` (yol gösterimi), gerekiyorsa `Sidebar.tsx`.
**Doğrulama:** e2e'de yol görünür + kopyalanabilir.
**Boyut:** S.

### 0.3 Donanım write-blocker beyanı — [ ]
Adli araç standardı: yazılım `GENERIC_READ` ile açıyor (yazma yok) ama profesyonel kullanım donanım write-blocker ister. Dashboard'a bilgi notu: "Kaynak diskler salt-okunur açılır; adli zincir için donanım write-blocker kullanılması önerilir" (TR/EN i18n).
**Doğrulama:** i18n anahtar parite testi; e2e'de metin görünür.
**Boyut:** S.

### 0.4 Saha turunu yürüt + bulgu triyajı — [ ]
Protokolü uygula (kullanıcının donanımıyla — bu adım kullanıcı işbirliği ister; plan icracısı hazırlar, koşturur, kaydeder). Çıkan her bulgu: `docs/field-test-2026-XX/` altına kayıt + mevcut fazlara yeniden önceliklendirme girdisi.
**Doğrulama:** her matris satırı için doldurulmuş kayıt (geçti/geçmedi + kanıt).
**Boyut:** M (süre donanıma bağlı).

**Faz kapısı:** en az (a),(b),(f) satırları geçmiş; kritik geçmezse ilgili faz maddesi öne alınır.

---

## FAZ 1 — Ölçek: 8TB / Milyon Kayıt Rejimi ( tahmini 1-2 hafta)

**Gerekçe:** Motor değerlendirmesinin en düşük alt-skoru (4/10) burası. 2M dosyalık NTFS taraması 1-3 GB RAM biriktiriyor; 1M+ kayıtta `LIMIT/OFFSET` doğrusal yavaşlıyor; 500 satır DOM'a düz basılıyor. Saha testi büyük diskte yapılacaksa bu faz önce bitmeli.

### 1.1 NTFS akışkan emit (full-RAM birikimin kaldırılması) — [ ]
**Tasarım:** `ntfs_parser.cpp` Pass-1 bugün tüm `tempFiles` vektörünü (FileRecord + residentData blob'ları) RAM'de tutuyor, Pass-2 isimlendiriyor. Yeni düzen: Pass-1 kompakt bir `MftIndex` üretir (ino → {offset, size, runs-packed, flags} — runs zaten `runs_codec` ile paketlenebilir), FileRecord'ları emit ETMEZ. Pass-2 her dizin bloğunu okuduğunda kaydı `MftIndex`'ten geri monteler ve callback'e akıtır. Resident data yalnız ikinci geçişte, o kayıt için okunur. Resume (`loadFromRecords`) yolu: DB'den geri hydrate yerine mevcut `MftIndex`磁盘'ten yeniden kurulabilir — ama dürüst yol: resume'da DB zaten kayıtları içeriyor, `DedupIndex` rehydrate'u yeterli; full re-hydrate kaldırılıp yalnız dedup+audit state yeniden kurulur.
**Dosyalar:** `native/src/fs/ntfs_parser.cpp` (büyük refactor), `native/include/byteback_fs.h`, testler `test_ntfs_parser.cpp`, `test_e2e_scan_recover.cpp` (bellek profili assert'i — Windows'ta `GetProcessMemoryInfo` ile peak working set üst sınırı, örn. 2M sentetik kayıtta < 700MB).
**Risk:** pass-2'de MFT kaydı yeniden okuma IO artışı (her kayıt için 1K okuma yerine dizin-blok-başına toplu okuma yapılmalı — dizin blokları zaten okunuyor, kayıt montajı `readAt` ile tekil; kabul edilebilir). Davranış eşdeğerliği: emit SIRASI değişebilir (multiset eşitliği, mevcut paralel carve prensibiyle uyumlu).
**Doğrulama:** mevcut NTFS testleri + yeni: 200K sentetik dosyalık hacimde kayıt sayısı birebir + peak RSS üst sınırı.
**Boyut:** L.

### 1.2 Keyset sayfalama — [ ]
**Tasarım:** `metadata_store.cpp` `getFiles` `LIMIT ? OFFSET ?` → `WHERE (sort_key, id) > (?, ?) LIMIT ?` composite keyset. Sort anahtarına göre 4-5 whitelist deseni (confidence+id, size+id, name+id, date+id, path+id). Renderer `toSqlListFilter`'a `cursor?: {keyValue, id}` ekler; "sonraki sayfa" cursor ister. OFFSET yolu geriye uyumluluk için kalır (CSV export).
**Dosyalar:** `native/src/db/metadata_store.cpp`, `bridge_scan.cpp` (GetFilesPage payload +cursor), `src/shared/ipc-contract.ts`, `src/renderer/components/ResultsView/*`.
**Doğrulama:** native test: 100K satırda son sayfa keyset <5ms (OFFSET 99500 ~9.5ms idi); renderer vitest cursor davranışı.
**Boyut:** M.

### 1.3 Sanallaştırılmış sonuç tablosu — [ ]
500 satır DOM yerine pencere içi satır render'ı (framework'süz: scroll listener + `Math.ceil(viewport/rowH)` buffer — YAGNI: react-window dependency'siz elle, ~80 satır). Thumbnail galeri: disk-cache (`userData/thumbs/` LRU 256MB cap; native preview API'den alınan blob'u dosyaya yaz, yeniden ziyarette data-URL yerine `file://`... hayır — CSP `img-src` `file:` içermez; çözüm: `protocol.registerFileProtocol` yerine custom `thumb://` şeması kaydet + CSP'ye `thumb:` ekle).
CSV export: 100 IPC turu yerine native'e `exportCsv(scanId, filter, destPath)` — sqlite3 `sqlite3_exec` callback ile akış yaz (dialog allowlist deseniyle hedef doğrulama main'de).
**Dosyalar:** `ResultsView.tsx` (ağır), yeni `src/renderer/components/ResultsView/VirtualRows.tsx`, `src/main/ipc-handlers.ts` + `bridge_ops.cpp` (csv), `main.ts` (thumb:// protocol), `index.html` CSP.
**Doğrulama:** e2e: 500 satır sayfalama davranışı aynen; vitest: virtual window hesabı saf fonksiyon testi.
**Boyut:** M+L (üç alt madde; tek task'ta değil, 1.3a/1.3b/1.3c olarak ayrı planlanır).

### 1.4 Kalan test-hijyen — [ ]
7 test dosyasına PID-unique temp pattern'i (0d8c63b'te şablon kuruldu: `byteback_*_test_<pid>`).
**Boyut:** S. **Doğrulama:** süit yeşil + iki paralel ctest örneği.

**Faz kapısı:** 2M sentetik kayıt senaryosu: RSS < 1GB, tarama-sonuç-sayfalama akıcı, CSV 100K kayıt < 10s.

---

## FAZ 2 — Silinmiş-Durup Derinliği ( tahmini 1-2 hafta)

**Gerekçe:** Rakip analizde "deleted state farkının büyük kısmı burada" tespiti. Foto kurtarma ürünü için en görünür değer.

### 2.1 NTFS $INDEX_ALLOCATION recarve — [ ]
Silinmiş klasör kayıtlarının INDEX blokları ($I30) serbest bölgede kalır; `ntfs_parser.cpp:745` "later" notu. Yaklaşım: carve aşamasında `INDX` magic'li blokları ayrıştır → FILE_NAME özniteliklerinden parent-ref zinciriyle silinmiş yol ağacı kur; mevcut path_rebuild ile birleştir.
**Doğrulama:** sentetik: silinmiş-klasör+ağaç fixture → silinmiş yollar doğru addlarla listelenir.
**Boyut:** M.

### 2.2 ext4 journal (jbd2) replay — [ ]
Yol haritası #12. `jbd2` log yürüyüşü: superblock (magic 0xc03b3998) → descriptor blokları → commit seq zinciri → son geçerli commit'e kadar transaction'ları bellekte yeniden oynat → silinmiş inode/blok referanslarını kurtar. Sadece OKUMA: orijinal FS asla değişmez, "replay" sanal bir snapshot üretir (adli doğru).
**Dosyalar:** yeni `native/src/fs/ext4_journal.cpp` + header; `scan_coordinator` entegrasyon koşulu: ext4 taramasında journal varsa ek kaynak olarak emit.
**Doğrulama:** hand-crafted jbd2 fixture (descriptor+commit+2 transaction) → silinmiş dosya journal'dan kurtarılır.
**Boyut:** L (format dikkat ister; spec cite: kernel `include/linux/jbd2.h`).

### 2.3 HFS+ unused catalog slot'ları — [ ]
`hfs_catalog.cpp`: leaf node kayıtları silinince slot "unused" işaretlenir ama içerik kalır. Node-record yürüyüşünde unused slot'ların baytlarını da parse et → silinmiş kayıt (conf 40-55 bandı, `status=0`).
**Doğrulama:** sentetik leaf: bir kayıt unused → emit edilir.
**Boyut:** S-M.

### 2.4 APFS deleted omap/snapshot taraması — [ ]
`apfs_container.cpp` genişletme: snapshot superblock'ları (nx_superblock zincirindeki `nx_xp_desc` ile) + silinmiş dosyaların omap'te kalıntı OID'leri. Kapsamı dürüst tut: v2 checkpoint'lerinde silinmiş j-file_oid → virtual oid eşleşmesi bulunduğunda emit.
**Doğrulama:** sentetik checkpoint fixture.
**Boyut:** M-L (APFS karmaşık; kısmi kapsamı dürüstçe belgele).

### 2.5 XFS silinmiş inode + CRC32C — [ ]
(a) Silinmiş inode: AG inode tablosunda magic'i bozulmuş/dağıtılmış kayıtlar yerine, `agi` free-list + unlinked hash (`iunlinked`) yürüyüşü →referanssız ama dolu inode'lar. (b) CRC32C doğrulama: vendored tablo (zlib'den bağımsız ~1KB table-driven) — v5 inode/sb/dir3/btree bloklarının `crc` alanı doğrulanır; uyuşmazlık = conf düşür, kayıt düşmez (adli: bozuk ama içerik var).
**Dosyalar:** `xfs_parser.cpp` + yeni `native/src/fs/xfs_crc.cpp`.
**Doğrulama:** CRC'li sentetik v5 inode: doğru crc geçer, bozuk crc conf düşürür.
**Boyut:** M.

### 2.6 Metadata kaynaklarında content-hash — [ ]
Carve-only olan content-hash'i metadata kaynaklarına genişlet: emit sırasında metadata kaydının ilk min(64KB) MD5'i hesaplanır (diskten okuma maliyeti — yalnız quick-scan sonunda, kullanıcı "Tekrarları analiz et" dediğinde tembel hesap daha iyi; tasarım kararı: tembel, talep-bazlı sütun). Cross-source dedup: sonuç görünümünde "aynı içerik: metadata + carve" birleştirme görünümü.
**Boyut:** M. **Doğrulama:** aynı içerik iki kaynaktan → tek gruplama.

**Faz kapısı:** her FS için "silinmiş dosya kurtarma oranı" sentetik vaka raporu.

---

## FAZ 3 — Format ve Kapsama Genişletme ( tahmini 1-2 hafta)

### 3.1 RAW koşullu imzalar: NEF/ARW/DNG — [ ]
Sabit-offset magic yok; TIFF magic + EXIF Make alanı koşullu. `signature_engine`'e "koşullu imza" kavramı: aday TIFF ise 64KB probe içinde `Make` tag'ini ara (Nikon/Sony/Adobe) → etiketle. DNG: `DNGVersion` tag'i varsa DNG.
**Doğrulama:** gerçek NEF/ARW/DNG örneklerinin ilk 64KB'siyle golden fixture (binary hex dizisi olarak test'e gömülü — lisansız küçük örnekler).
**Boyut:** M.

### 3.2 X3F boyut sınırı — [ ]
Dizin işaretçisi dosya sonunda: carve adayında disk-sonuna targeted read (`reader->readBytes(fileEnd-4..)`) → dizin → en büyük section bound. CA-001 ihlali değil: okuma bounded (tek sektör+), aday zaten magic'le bulunmuş. EOF'ta sector read'in imaj-sonu clamp'i zaten var.
**Boyut:** S-M.

### 3.3 VHD/VHDX açma — [ ]
Yol haritası #3'ün Windows yarısı (önce): VHD: footer (magic "conectix" @-512) + BAT; VHDX: header+batı region tabloları. `ByteSource` arayüzüne `VhdSource`/`VhdxSource` — sparse block'lar talep-bazlı çözülür, `DiskReader::attachVhd(path)`; `clone()` kendi path'ini yeniden açar (paralel carve uyumlu!).
**Doğrulama:** PowerShell `New-VHD`/`Convert-VHD` ile üretilmiş gerçek dosya formatında round-trip (imaj içine mini FAT koy, tara).
**Boyut:** M-L.

### 3.4 VMDK/VDI açma — [ ]
VMDK sparse (COWD/SESparse değil, host dense + sparse extent), VDI mapped block tablosu. 3.3'ün arkasında aynı desen.
**Boyut:** M-L.

### 3.5 Btrfs ayrıştırıcı — [ ]
Superblock @64KiB magic "_BHRfS_M"; chunk-tree yürüyüşü → logical↔physical mapping; extent-tree'den silinmiş extent kalıntıları (ref count 0 ama metadata'sı duran). Karmaşık: önce salt-okunur canlı ağaç (dosya listeleme), silinmiş-durup 2. fazı.
**Boyut:** L (XFS'ten büyük; chunk mapping gerçek iş).

### 3.6 RAID5 şerit boyutu kesinleştirme — [ ]
Parity tek-başına underdetermined: kazanan adayların her biri için FS-imza tutarlılık skoru (aday geometriyle birleştir → 0. sektörde boot magic ara; NTFS/FAT superblock'ları doğru yerde mi). `raid_detect.cpp`'te ikinci aşama skorlama.
**Doğrulama:** sentetik: RAID5 64K gerçek + 128K aday da parity-uyumlu → FS-imza skor 64K'ı seçer.
**Boyut:** M.

**Faz kapısı:** desteklenen format matrisi güncellenir (README + Dashboard FS rozetleri).

---

## FAZ 4 — İnceleyici Araçları ( tahmini 1 hafta)

### 4.1 Hex editör disk-geneli arama — [ ]
Yol haritası #7. Native: `searchDiskHex(driveIndex, pattern[]|ascii, {maxHits, isRunning})` → AsyncWorker + TSFN progress; pattern AC tablosuyla değil plain memchr+memcmp (tek pattern); sector-yürüyüş mevcut carve altyapısını kullanır. Renderer: HexEditor'a arama çubuğu + sonuç listesi (sektör+ofset) + tıkla-zıpla; imlek (bookmark) kalıcı: `userData/hex-marks.json`.
**Doğrulama:** sentetik hacimde çoklu vuruş listesi + e2e zıplama.
**Boyut:** M.

### 4.2 MFT kayıt görüntüleyici — [ ]
#8: `getMftRecord(driveIndex, recordNo)` → ham 1KB + parse edilmiş görünüm (header alanları, attribute listesi: tip/ad/boyut/resident). Renderer: modal/tab şablon görünümü; sonuç tablosundan "MFT kaydını göster" bağlam menüsü.
**Doğrulama:** sentetik MFT kaydı alan-değil golden assert.
**Boyut:** M.

### 4.3 Ses önizleme — [ ]
#9: MP3/WAV/OGG için `<audio controls src=dataUrl>` — mevcut preview API mime tipini zaten taşıyor; eksikse `preview_reader`'a ses uzantıları + `audio/*` mime. ffmpeg WAV-proxi YAGNI (ilk sürüm doğrudan data-URL; 64KB cap'i ses için 2MB'a çıkar, stream değil — ceiling notu).
**Boyut:** S.

### 4.4 Dosya onarım (kurtarma-sonrası opsiyonel adım) — [ ]
#10, dürüst kapsamla: (a) JPEG: eksik EOI ekleme + kesik scan'de JPEG tamamlama (kurtarılan dosya "partial" durumundaysa düzeltme öner); (b) ZIP: merkez dizini yeniden inşa (local header taramasıyla — carve motorundaki zip bilgisini yeniden kullan); (c) PDF xref rebuild (startxref tespiti + `%EOF` taraması). Her biri "onarıldı" değil "onarılmaya çalışıldı, fark: +N bayt / yeni MD5" diye raporlanır — adli dürüstlük: orijinal kurtarılan dosya ASLA değişmez, onarık kopya ayrı dosyadır (`_repaired` ek adı).
**Boyut:** M (üç bağımsız alt madde; ayrı planlanır).

**Faz kapısı:** uzman kullanıcı senaryosu e2e'si: sonuçlar → hex'te bul → MFT kaydı gör → önizle → kurtar → onar.

---

## FAZ 5 — Üretim Sertleştirme ve Dağıtım ( tahmini 3-5 gün + imza süreci)

### 5.1 Kod imzası — [ ]
Seçenek analizi (kullanıcı kararı ister): (a) Azure Trusted Signing (bireysel uygunluk belirsiz), (b) OV sertifikası (~$200-400/yıl, Sectigo/SSL.com), (c) self-signed + kurulum talimatı (geçici). electron-builder `win.sign`/`signtool` entegrasyonu; CI'a secret. **Bu madde kullanıcı onayı olmadan ilerlemez.**
**Boyut:** süreç (S teknik, prosedür M).

### 5.2 Unicode hedef yolları — [ ]
`disk_imager.cpp` + recovery yazımları dar `std::ofstream`/`CreateFileA` → geniş karakter (`std::ofstream` yerine `CreateFileW` sarmalayıcı ya da `std::filesystem::path` ile native). Türkçe karakterli hedef klasörlerde sessiz başarısızlık bilinen tavan.
**Doğrulama:** `C:\Çıkış\İmajlar\` hedefli test.
**Boyut:** M (kod tabanı geneli tarama ister).

### 5.3 HTTP HEAD-less fallback — [ ]
`byte_source.cpp`: HEAD 405/501 → `Range: bytes=0-0` GET ile boyut+206 kontrolü; sonra normal akış.
**Boyut:** S.

### 5.4 posix RAID okuma dalı — [ ]
`disk_reader_posix.cpp` `readSectors`'a `raidBackend_` dalı (win'dekiyle aynı mantık). CI syntax job kapsıyor; işlevsel test yalnızca Linux makinede — dürüst not: bu makinede runtime kanıtı yok.
**Boyut:** S.

### 5.5 Kararlılık kararları — [ ]
(a) Auto-update: adli araç + offline kullanım → **kapalı tutma kararı** dokümante (electron-updater entegrasyonu değil); manuel güncelleme talimatı README. (b) Crash raporlama: harici telemetry YOK (adli); yerel crash-dump (`app.setPath('crashDumps')` + son crash'ı Dashboard'da göster — session-log zaten var, tek eklenti: `crashReporter` yerel-mod).
**Boyut:** S.

### 5.6 Sürüm 1.0 ölçütleri — [ ]
`docs/release-checklist.md`: tüm faz kapıları + saha testi turu + imza + installer doğrulama + README/CHANGELOG güncelleme. v0.1.0 → v1.0.0 etiketi bu checkliste bağlı.
**Boyut:** S.

---

## Kapsam dışı (bilinçli, belgeli tavanlar)

- **ZFS**: bileşik pool/omap mimarisi — uzman sürüm kapsamı (rekabet doküsüyle uyumlu). Btrfs (3.5) bitmedikçe başlanmaz.
- **Ağ üzerinden kurtarma (#11)**: agent/istemci modeli ayrı ürün büyüklüğünde; talep gelirse ayrı plan.
- **F2FS/UFS**: mobil/niche; Btrfs'ten sonra önceliklendirmesi veriye bağlı.
- **LZNT1 >64MB**: decompressor'ın tasarım sınırı; RtlDecompressBuffer'ın kendisi de sınırlı — yeterli.
- **SSD fiziksel disklerde paralel carve**: seek-bound politika kararı değişmedi.
- **smart_monitor.cpp / data_shredder.cpp posix portu**: Windows özellikleri (`<windows.h>` gereksinimli); CI syntax job'unda belgeli dışlama. Linux portu talep gelirse ayrı plan.

## Yürütme disiplini

1. Her faz başlangıcında: fazın detay görev planı `writing-plans` formatında üretilir (test kodu dahil adımlar) — bu belge o planın spec'idir.
2. Bağımsız maddeler çok-lane paralel ajanlarla yürütülür (yerleşik ritual: sahipli dosyalar + private build dir + merkezî kapılar).
3. Her commit kapıları yeşil; her faz sonunda saha-protokol ilgili satırları yeniden koşulur.
4. Toplam tahmini takvim: fazlar tam seri ~5-7 hafta; paralel kol çalışmasıyla 3-4 hafta gerçekçi.
