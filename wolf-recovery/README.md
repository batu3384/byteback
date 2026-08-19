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
- **HFS+ / APFS**: HFS+ katalog B-tree (25k tavan; kesince `hfs_limit`
  sentinel + denetim olayı). APFS NXSB + volume keşfi — katalog/omap yok.
- **Carving**: Aho-Corasick imza taraması (~114 gömülü imza), yapısal
  doğrulama (JPEG/PNG/ZIP/PDF/GZIP/RIFF), iki parçalı dosyalar için
  kümelenmiş BGC kurtarma yolu (sektör adımlı, tarama başına bütçeli).
- **RAID**: 0/1/5/6/10 — GF(2⁸) Reed-Solomon çift parite, sektör hizalı
  okuma, bozuk sektörde sıfır doldurma (tarama asla düşmez).
- **İmajlama**: RAW (dd) ve **E01 (EWF)** — yazım sırasında MD5. E01 tek
  segment; tablo `uint32` (~4 GiB chunk tavanı).
- **SMART**: ATA öznitelikleri + NVMe Health Info Log, SSD/TRIM uyarısı.
  ATA skoru KALİBRASYONSUZ heuristic.
- **NSRL**: kullanıcı seçimli metin/CSV MD5 seti (bellek içi). Gömülü RDS yok.
- **Denetim**: SHA-256 hash zincirli adli günlük (RFC 6234 vektörlü).

### Arayüz (Electron + React)
Dashboard, canlı tarama + kötü sektör haritası, dizin ağacı + dosya detay
bölmesi, hex inceleyici (entropi + veri şablonları), RAW/E01 imajlayıcı
(MD5 bütünlük paneli), SMART paneli, sanal RAID kurucu, USN olay zaman
çizelgesi, CSV dışa aktarım, SHA-256 özetli HTML/PDF adli rapor, dava/NSRL
formu, açık/koyu tema.

## Proje Yapısı

```
wolf-recovery/
├── .github/workflows/     # CI: native + electron build, testler, ewfinfo kapısı
├── docs/
│   ├── ARCHITECTURE.md    # katman/modül haritası, doğrulama öyküsü
│   └── codebase-audit/    # denetim raporları (md + json sidecar)
├── resources/
│   ├── signatures.json    # isteğe bağlı kullanıcı imza seti (yoksa gömülü tablo)
│   └── icon.svg           # ürün işareti
├── native/
│   ├── include/           # başlıklar (fs/, carver/, crypto/, imager/, ...)
│   ├── src/               # motor kaynakları (modül tablosu ARCHITECTURE.md'de)
│   ├── tests/             # GoogleTest — sayı için `ctest -C Release`
│   └── third_party/       # vendored sqlite3
├── src/
│   ├── main/              # Electron main + IPC + native köprüsü
│   ├── preload/           # contextBridge (güvenli beyaz liste API)
│   ├── renderer/          # React bileşenleri (görünüm başına klasör)
│   └── shared/            # paylaşılan tipler + saf yardımcılar
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
2. **Faz 7 (inceleyici yüzeyi)** — Case/NSRL sayfası, HFS tavan uyarısı,
   ATA KALİBRASYONSUZ etiketi, README/ctest hizası.
3. **Denetim** — `docs/codebase-audit/` tarihli raporlar yaşayan belgedir.
   Bir koşunun "tamamı giderildi" iddiası sonraki koşuyu kapatmaz.

**Bilinen sınırlar (bilinçli, etiketli):** BitLocker şifre çözme (tespit var),
APFS omap/katalog yok (NXSB + volume keşfi), `$LogFile` redo yok, GPU PFAC yok,
NSRL tam RDS/on-disk indeks yok (bellek seti), Weibull ATA modeli
KALİBRASYONSUZ, E01 çoklu segment yok (~4 GiB tek segment), HFS katalog 25k,
içerik FTS örnek tavanı 256 KB.

## Güvenlik Notları
- Uygulama sektör erişimi için Yönetici gerektirir (NSIS manifestı
  `requireAdministrator`); motor diske asla yazmaz.
- Disk geneli boş-alan imhası, dosya sistemi farkında bir uygulama
  yazılana kadar bilinçli olarak devre dışıdır; yerel imha yalnızca tek
  dosya yollarını kabul eder.
- NSRL yolu renderer'dan gelmez; main-process dosya diyaloğu seçer.

## Lisans
MIT License
