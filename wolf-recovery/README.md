# Wolf Recovery

**Profesyonel adli bilişim ve veri kurtarma aracı.** Yüksek performanslı Windows C++ motorunu modern bir Electron/React arayüzüyle birleştirir. Windows API'lerini atlayarak doğrudan ham disk sektörlerini okur; NTFS dosya sistemi yapılarından ve dosya imzalarından (carving) kayıp dosyaları kurtarır.

Hibrit konumlandırma: hem **adli bilişim** (Autopsy/X-Ways seviyesinde kanıt bütünlüğü, zincirleme sorumluluk, E01 imajlama) hem de **veri kurtarma** (R-Studio/DMDE seviyesinde RAID onarımı, bölüm kurtarma, SSD/TRIM farkındalığı) hedefler.

## Özellikler

### Çekirdek Motor (C++)
- **Doğrudan ham disk erişimi** — `DeviceIoControl` ile Windows fiziksel sürücü erişimi
- **MFT (Master File Table) parsing** — NTFS dosya kurtarma
- **İmza tabanlı carving** — Aho-Corasick otomatı, ~100 dosya imzası
- **SQLite destek deposu** — WAL modu, sayfalama, batch insert
- **SMART izleme** — ATA/SATA + Weibull arıza tahmini
- **Disk imajlama** — RAW/DD formatı
- **Veri parçalama (shredding)** — DoD 5220.22-M
- **Adli denetim günlüğü** — SHA-256 hash zinciri

### Arayüz (Electron/React)
- Dashboard, Sürücü kartları
- Canlı tarama görünümü + disk haritası
- Hex editör + entropi analizi + veri şablonları
- Sonuçlar görünümü (filtreleme, sayfalama)
- SMART sağlık görünümü
- RAID yapılandırıcı (drag & drop)
- Disk imajlama + I/O gecikme grafiği
- Anahtar kelime arama
- Rapor oluşturucu
- Veri parçalayıcı

## Teknoloji Yığını
| Bileşen | Teknoloji |
| --- | --- |
| Arayüz | React 18, TypeScript, Vite |
| Masaüstü | Electron, IPC Main/Renderer |
| Backend Motor | C++17, Windows API'leri (DeviceIoControl) |
| Native Bridge | Node-API (N-API), cmake-js |
| Veritabanı | SQLite (vendored) |
| Derleme | CMake 3.20+, Visual Studio 2022 |

## Gereksinimler
- Visual Studio 2022 Build Tools ("Desktop development with C++" iş yüküyle)
- Node.js 20+
- CMake 3.20+

## Kurulum ve Çalıştırma
```bash
# Depoyu klonla
git clone <repo-url>
cd wolf-recovery

# Node bağımlılıklarını yükle
npm install

# Native motoru derle
npm run build:native

# Uygulamayı geliştirme modunda başlat
npm run dev
```

## Proje Yapısı
```
wolf-recovery/
├── src/
│   ├── main/              # Electron main process (yaşam döngüsü, IPC)
│   │   ├── main.ts
│   │   ├── ipc-handlers.ts
│   │   └── native-bridge.ts
│   ├── preload/
│   │   └── index.ts       # contextBridge (güvenli IPC)
│   ├── shared/
│   │   └── types.ts       # Paylaşılan TypeScript tipleri
│   └── renderer/
│       ├── App.tsx        # Ana uygulama + sayfa yönlendirme
│       └── components/    # UI bileşenleri (görünüme göre klasörlü)
│           ├── Dashboard/
│           ├── ScanView/
│           ├── ResultsView/
│           ├── HexEditor/
│           ├── ImagerView/
│           ├── SmartView/
│           ├── VirtualRaid/
│           ├── DiskMap/
│           ├── SearchView/
│           ├── ShredderView/
│           ├── ReportView/
│           └── Layout/
├── native/
│   ├── include/           # C++ başlık dosyaları (modül bazlı)
│   │   ├── fs/            # Dosya sistemi parser'ları
│   │   ├── math/          # Matematik yardımcıları
│   │   └── forensic/      # Adli bilişim modülleri
│   ├── src/
│   │   ├── bridge/        # N-API native add-on bağlayıcıları
│   │   ├── fs/            # NTFS/FAT/exFAT/ext4/HFS+/APFS parser'ları
│   │   ├── carver/        # Aho-Corasick imza motoru
│   │   ├── io/            # Ham disk I/O (Windows)
│   │   ├── db/            # SQLite metadata deposu
│   │   ├── smart/         # SMART izleme
│   │   ├── imager/        # Disk imajlama
│   │   ├── recovery/      # Dosya kurtarma motoru
│   │   ├── validator/     # Dosya doğrulama
│   │   ├── security/      # Veri parçalama
│   │   ├── forensic/      # Adli denetim günlüğü
│   │   └── memory/        # Bellek havuzu
│   ├── third_party/       # sqlite3.c (vendored)
│   └── CMakeLists.txt     # CMake yapılandırması
└── package.json           # Proje meta verisi ve derleme scriptleri
```

## Yol Haritası
Master geliştirme planının durumu:

1. **Faz 0** ✅ — Temizlik, stabilite, bug-fix, CI/CD, test altyapısı
2. **Faz 1** ✅ — NTFS derinleştirme (UTF-16, USA fixup, zaman damgaları, sparse run, LZNT1, ADS, USN journal, dizin + INDX slack)
3. **Faz 2** ✅ — Tiered carving (Fast Object Validation: JPEG/PNG/ZIP/PDF/GZIP; Bifragmented Gap Carving; ~114 imza)
4. **Faz 3** ✅ — RAID 0/1/5/6/10 (GF(2⁸) çift-parite) + E01 (EWF) adli imajlama + on-the-fly MD5
5. **Faz 4** ✅ — Çoklu dosya sistemi (exFAT entry-set, Ext4 extent tree + dirents) + bölüm-farkındalıklı tarama (MBR/GPT)
6. **Faz 5** ✅ — NVMe SMART (Health Info Log) + ATA IDENTIFY + SSD/TRIM farkındalığı
7. **Faz 6** ✅ (temel) — Unified Timeline (USN journal → timeline_events → UI) + BitLocker tespiti
8. **Faz 7** ✅ (temel) — Dürüst rapor (gerçek SHA-256 + gömülü denetim günlüğü), PDF export, CSV export, dizin ağacı, dosya önizleme, ışık teması, NSIS paketleme

Yapılacaklar (tech debt): VSS snapshot analizi, BitLocker şifre çözme (AES-XTS), artifact ingest
(browser/registry/email), APFS/HFS+ tam parser, $LogFile, GPU PFAC, NSRL hash setleri.

## Geliştirme
```bash
# Tip kontrolü (renderer + main)
npm run typecheck

# Üretim derlemesi (out/ klasörü)
npm run build

# Testler
npm run test          # Vitest (renderer)
npm run test:native   # GoogleTest (native, 66 test)

# Windows kurulum paketi üret (release/ altına NSIS installer)
npm run dist
```

## Lisans
MIT License
