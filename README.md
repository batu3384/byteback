# Byteback

**Windows için profesyonel adli bilişim + veri kurtarma aracı.** Ham disk
erişimini yerel bir C++ motoruyla, modern bir Electron/React arayüzüyle
birleştirir. Motor diski yalnızca okur (`GENERIC_READ`); yazma işlemi
kullanıcının seçtiği hedef dosyalara yapılır.

Hibrit konumlandırma: hem **adli bilişim** (E01 imajlama, hash zincirli
denetim günlüğü, USN zaman çizelgesi, rapor bütünlük özeti) hem de **veri
kurtarma** (MFT/FAT/ext4 kurtarma, imza carving, RAID 0/1/5/6/10 sanal
yeniden kurulum, SSD/TRIM farkındalığı).

## Özellikler

### Çekirdek Motor (C++17)
- **NTFS**: UTF-16 dosya adları, USA fixup, $STANDARD_INFORMATION zaman
  damgaları, sparse data run'ları, LZNT1 açma, ADS (alternatif akıtlar),
  USN journal ayrıştırma, INDX slack taraması, dizin ağacı kurulumu.
  Hızlı tarama: boot `$MFT` LCN run yürüyüşü. Derin: yetim FILE carve.
- **FAT12/16/32 + exFAT**: FAT zincir yürüyüşü (döngü korumalı), VFAT uzun
  adlar, exFAT entry-set durum makinesi, DOS zaman damgaları.
- **Ext2/3/4**: extent tree (derinlik destekli), directory entry'lerden
  gerçek dosya adları, silinmiş inode/dirent kanıtı.
- **ReFS**: boot/SUPB tanıma, ministore metadata walk, integrity-stream
  CRC64-ECMA doğrulama (SUPB self-check + resident dosya trailer).
- **E01 okuma**: yerel `.E01` (çok segment) + HTTP Range ham imaj; `attachEwfImage` /
  `attachHttpRawImage` / `attachRawFile` DiskReader API.
- **HFS+ / APFS**: HFS+ katalog B-tree (iptal edilene kadar). APFS NXSB
  (`nx_block_size`, `nx_fs_oid[100]`), APSB volume, btree yaprak drec
  (`source=apfs_file`, keşif) ve file extent (`apfs_extent`, run ile kurtarma).
  Katalog: nx_fs_oid + ilk 256 blok + omap yaprak paddr (tam container walk yok).
- **Carving**: Aho-Corasick imza taraması (~114 gömülü imza), yapısal
  doğrulama (JPEG/PNG/ZIP/PDF/GZIP/RIFF), iki parçalı dosyalar için
  kümelenmiş BGC kurtarma yolu (sektör adımlı, tarama başına bütçeli).
  CPU only; CUDA/OpenCL PFAC yok.
- **RAID**: 0/1/5/6/10 — GF(2⁸) Reed-Solomon çift parite, sektör hizalı
  okuma. Bozuk üye veya I/O hatası: RAID 0 o aralığı sıfırlar (tarama
  düşmez); RAID 1 ayna dener; RAID 5/6 parite ile okur. Üye `fail_disk`
  ile işaretlenir (NAPI + RAID ekranı).
- **İmajlama**: RAW (dd) ve **E01 (EWF)** — yazım sırasında MD5. E01 çok
  segment (.E01 → .E02); segment başına uint32 tablo.
- **SMART**: ATA öznitelikleri + NVMe Health Info Log, SSD/TRIM uyarısı.
  ATA skoru ACS defect sayacı (realloc 0x05, pending 0xC5).
- **NSRL**: kullanıcı seçimli metin/CSV MD5 seti (SQLite indeks). Gömülü RDS yok.
- **Denetim**: SHA-256 hash zincirli adli günlük (RFC 6234 vektörlü).

### Arayüz (Electron + React)
Dashboard, canlı tarama + kötü sektör haritası, dizin ağacı + dosya detay
bölmesi, hex inceleyici (entropi + veri şablonları), RAW/E01 imajlayıcı
(MD5 bütünlük paneli), SMART paneli, sanal RAID kurucu, USN olay zaman
çizelgesi, CSV dışa aktarım, SHA-256 özetli HTML/PDF adli rapor, dava/NSRL
formu, açık/koyu tema.

## Proje Yapısı

```
byteback/                  # GitHub repo kökü (klonladığınız dizin)
├── .github/workflows/     # CI
├── docs/                  # ARCHITECTURE, denetim raporları, yol haritası
├── native/                # C++17 motor + GoogleTest
├── src/                   # Electron main, preload, React renderer
├── e2e/                   # Playwright duman testleri
├── resources/             # İkon, imza JSON
├── package.json
└── README.md
```

> **Not:** Yerel klasör adınız (`disk`, `byteback`, vb.) önemli değil; ürün kodu
> repo kökündedir. GitHub’da repo adını `byteback` yapmanız önerilir.

## Gereksinimler
- Visual Studio 2022 Build Tools ("Desktop development with C++")
- Node.js 20+
- CMake 3.20+

## Kurulum ve Çalıştırma
```bash
git clone <repo-url>
cd byteback    # veya klonladığınız klasör adı
npm install
npm run dev    # native derle + geliştirme oturumu
```

## Komutlar
```bash
npm run build:native   # C++ motoru derle (cmake-js)
npm run build          # native + electron-vite üretim derlemesi
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer/shared)
npm run test:native    # GoogleTest via `ctest -C Release`
npm run test:e2e       # Playwright Electron duman (`out/main/main.js`)
npm run dist           # NSIS x64 kurulum paketi (release/)
```

> Not: `build:native` test üretecinin önbelleğini sıfırlar; `test:native`
> öncesinde `cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON` ile
> yeniden yapılandırın. Test sayısı `ctest -C Release` çıktısına bağlıdır;
> `Ewf.OptionalEwfinfoCrossCheck` `BYTEBACK_EWFINFO` yoksa skip olur.

## Benchmark (geliştirici)

| Senaryo | Ortam | Not |
|---------|-------|-----|
| 64 MiB memory volume, deep scan | `BYTEBACK_RUN_BENCH=1 ctest -R BenchScan` | Programatik disk; gerçek SSD/HDD değil |
| 500 GB sparse deep scan | Planlanıyor | CI süresi için henüz otomatik değil |

Sonuçlar makineye bağlıdır; tablo dürüst referans içindir, pazarlama iddiası değil.

## Yol Haritası Durumu
1. **Faz 0–6 (motor)** — güvenilirlik, NTFS derinliği, VSS, RAID/batch,
   içerik FTS, Apple FS, ops (case SQLite + NSRL NAPI).
2. **Faz 7 (inceleyici yüzeyi)** — Case/NSRL sayfası, ATA ACS etiketi,
   README/ctest hizası.
3. **Denetim** — `docs/codebase-audit/` tarihli raporlar yaşayan belgedir.
   Bir koşunun "tamamı giderildi" iddiası sonraki koşuyu kapatmaz.

**Kalan spek sınırı (yanlış kripto/ürün yok):** BitLocker recovery password yalnızca **0x0800 clear-key** koruyucusu (TPM/startup-key/password → açık hata). FVEK: 64/128 hex veya recovery password → AES-128/256-XTS tarama+kurtarma+hex (imaj ham ciphertext). GPU PFAC yok. APFS: nx_fs_oid + 256 blok + **recursive omap btree** (tam snapshot tree değil). PhysicalDrive: seri + **IMHA** + OS onay; SSD ≠ NIST 800-88. Eşzamanlı scan/imaj/wipe engellenir.

## Güvenlik Notları
- Uygulama sektör erişimi için Yönetici gerektirir (NSIS manifestı
  `requireAdministrator`). Renderer `sandbox` + `contextIsolation`; native
  addon yalnız main süreçte.
- Motor kaynak diske `GENERIC_READ` ile açılır. Önce `FILE_SHARE_READ`;
  birim kilitlerse `FILE_SHARE_WRITE` yedek. Bu yazma izni değildir;
  donanım yazma engelleyici yoksa kanıt diski host OS değiştirebilir.
- Kurtarma ve imaj çıktısı kullanıcının seçtiği hedefe yazılır. Recover
  renderer `runs` kabul etmez; SQLite `fileId` + `scanId` şart.
- Disk geneli PhysicalDrive imhası: Yok Edici’de sürücü seç + seriyi yaz +
  OS onay. Native katman seri eşleşmeden yazmaz. Dosya/boş alan wipe hâlâ
  aygıt yollarını reddeder (CA-001). SSD’de DoD NIST sanitization değildir.
- NSRL yolu renderer'dan gelmez; main-process dosya diyaloğu seçer.

## Lisans
MIT License
