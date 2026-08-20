# Wolf Recovery

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
- **FAT12/16/32 + exFAT**: FAT zincir yürüyüşü (döngü korumalı), VFAT uzun
  adlar, exFAT entry-set durum makinesi, DOS zaman damgaları.
- **Ext2/3/4**: extent tree (derinlik destekli), directory entry'lerden
  gerçek dosya adları, silinmiş inode/dirent kanıtı.
- **HFS+ / APFS**: HFS+ katalog B-tree (iptal edilene kadar). APFS NXSB
  (`nx_block_size`, `nx_fs_oid[100]`), APSB volume, btree yaprak drec
  (`source=apfs_file`).
- **Carving**: Aho-Corasick imza taraması (~114 gömülü imza), yapısal
  doğrulama (JPEG/PNG/ZIP/PDF/GZIP/RIFF), iki parçalı dosyalar için
  kümelenmiş BGC kurtarma yolu (sektör adımlı, tarama başına bütçeli).
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
disk/
├── .github/workflows/     # CI: typecheck + Windows native/Electron/vitest
└── wolf-recovery/
    ├── docs/
    │   ├── ARCHITECTURE.md
    │   └── codebase-audit/
    ├── native/
    ├── src/
    ├── package.json
    └── README.md
```

## Gereksinimler
- Visual Studio 2022 Build Tools ("Desktop development with C++")
- Node.js 20+
- CMake 3.20+

## Kurulum ve Çalıştırma
```bash
git clone <repo-url>
cd wolf-recovery
npm install
npm run dev            # native derle + geliştirme oturumu
```

## Komutlar
```bash
npm run build:native   # C++ motoru derle (cmake-js)
npm run build          # native + electron-vite üretim derlemesi
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer/shared)
npm run test:native    # GoogleTest via `ctest -C Release`
npm run dist           # NSIS x64 kurulum paketi (release/)
```

> Not: `build:native` test üretecinin önbelleğini sıfırlar; `test:native`
> öncesinde `cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON` ile
> yeniden yapılandırın. Test sayısı `ctest -C Release` çıktısına bağlıdır;
> `Ewf.OptionalEwfinfoCrossCheck` `WOLF_EWFINFO` yoksa skip olur.

## Yol Haritası Durumu
1. **Faz 0–6 (motor)** — güvenilirlik, NTFS derinliği, VSS, RAID/batch,
   içerik FTS, Apple FS, ops (case SQLite + NSRL NAPI).
2. **Faz 7 (inceleyici yüzeyi)** — Case/NSRL sayfası, ATA ACS etiketi,
   README/ctest hizası.
3. **Denetim** — `docs/codebase-audit/` tarihli raporlar yaşayan belgedir.
   Bir koşunun "tamamı giderildi" iddiası sonraki koşuyu kapatmaz.

**Kalan spek sınırı (yanlış kripto/ürün yok):** BitLocker volume **şifre
kırma yok** — FVE metadata parse + AES-XTS NIST vektörleri; FVEK/48 haneli
anahtar yoksa birim şifreli kalır. GPU PFAC yok (taşınabilir CPU
Aho-Corasick). Playwright E2E yok (ctest + vitest). PhysicalDrive wipe yok
(CA-001; boş alan filler klasör ile).

## Güvenlik Notları
- Uygulama sektör erişimi için Yönetici gerektirir (NSIS manifestı
  `requireAdministrator`). Renderer `sandbox` + `contextIsolation`; native
  addon yalnız main süreçte.
- Motor kaynak diske `GENERIC_READ` ile açılır. Windows birim yöneticisi
  için paylaşım `FILE_SHARE_READ|FILE_SHARE_WRITE` kalır — bu yazma izni
  değildir; donanım yazma engelleyici yoksa kanıt diski host OS değiştirebilir.
- Kurtarma ve imaj çıktısı kullanıcının seçtiği hedefe yazılır. Recover
  renderer `runs` kabul etmez; SQLite `fileId` + `scanId` şart.
- Disk geneli PhysicalDrive imhası kapalı. Boş alan wipe, kullanıcının
  seçtiği klasörün biriminde filler dosya + DoD 3 geçiş.
- NSRL yolu renderer'dan gelmez; main-process dosya diyaloğu seçer.

## Lisans
MIT License
