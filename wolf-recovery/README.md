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
- **Carving**: Aho-Corasick imza taraması (~114 gömülü imza), yapısal
  doğrulama (JPEG/PNG/ZIP/PDF/GZIP/RIFF), iki parçalı dosyalar için
  kümelenmiş BGC kurtarma yolu (sektör adımlı, tarama başına bütçeli).
- **RAID**: 0/1/5/6/10 — GF(2⁸) Reed-Solomon çift parite, sektör hizalı
  okuma, bozuk sektörde sıfır doldurma (tarama asla düşmez).
- **İmajlama**: RAW (dd) ve **E01 (EWF)** — yazım sırasında MD5.
- **SMART**: ATA öznitelikleri + NVMe Health Info Log, SSD/TRIM uyarısı.
- **Denetim**: SHA-256 hash zincirli adli günlük (RFC 6234 vektörlü).

### Arayüz (Electron + React)
Dashboard, canlı tarama + kötü sektör haritası, dizin ağacı + dosya detay
bölmesi, hex inceleyici (entropi + veri şablonları), E01 imajlayıcı (MD5
bütünlük paneli), SMART paneli, sanal RAID kurucu, USN olay zaman
çizelgesi, CSV dışa aktarım, SHA-256 özetli HTML/PDF adli rapor, açık/koyu
tema.

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
│   ├── tests/             # GoogleTest — 86 birim testi
│   └── third_party/       # vendored sqlite3
├── src/
│   ├── main/              # Electron main + IPC + native köprüsü
│   ├── preload/           # contextBridge (güvenli beyaz liste API)
│   ├── renderer/          # React bileşenleri (görünüm başına klasör)
│   └── shared/            # paylaşılan tipler + saf yardımcılar (entropy)
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
npm run test           # Vitest (renderer, 10 test)
npm run test:native    # GoogleTest (86 test)
npm run dist           # NSIS x64 kurulum paketi (release/)
```

> Not: `build:native` test üretecinin önbelleğini sıfırlar; `test:native`
> öncesinde `cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON` ile
> yeniden yapılandırın.

## Yol Haritası Durumu
1. **Faz 0-7 (çekirdek kapsam)** ✅ — temizlik/stabilite, NTFS derinleştirme,
   tiered carving, RAID+E01, çoklu FS, NVMe SMART, timeline+BitLocker
   tespiti, dürüst raporlama + paketleme.
2. **İki denetim turu** ✅ — 15 + 11 bulgunun tamamı giderildi
   (`docs/codebase-audit/`); kritik matematiğin tamamı birim testli.

**Bilinen sınırlar (bilinçli, etiketli):** VSS snapshot analizi, BitLocker
şifre çözme (tespit var), APFS/HFS+ tam ayrıştırıcı, $LogFile, GPU PFAC,
NSRL hash setleri, Weibull parametre kalibrasyonu (model KALİBRASYONSUZ
etiketli), E01 çıktısının bağımsız okuyucu (libewf) ile çapraz doğrulaması
(CI'da araç varsa çalışan opsiyonel kapı mevcut).

## Güvenlik Notları
- Uygulama sektör erişimi için Yönetici gerektirir (NSIS manifestı
  `requireAdministrator`); motor diske asla yazmaz.
- Disk geneli boş-alan imhası, dosya sistemi farkında bir uygulama
  yazılana kadar bilinçli olarak devre dışıdır (denetim CA-001 kararı);
  yerel imha yalnızca tek dosya yollarını kabul eder.

## Lisans
MIT License
