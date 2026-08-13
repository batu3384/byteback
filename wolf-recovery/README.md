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
Master geliştirme planı, projeyi endüstri lideri seviyesine taşımak için 8 fazdan oluşur:

1. **Faz 0** ✅ — Temizlik, stabilite, bug-fix, CI/CD, test altyapısı
2. **Faz 1** — NTFS derinleştirme ($UsnJrnl, $LogFile, sparse MFT, ADS, UTF-16, ghost-MFT carving)
3. **Faz 2** — Tiered carving (Fast Object Validation, Bifragmented Gap Carving) + GPU PFAC + imza genişletme (400+)
4. **Faz 3** — RAID onarımı (6/10, Storage Spaces, LVM2, auto-detect) + E01/AFF4 adli imajlama + on-the-fly hashing
5. **Faz 4** — Çoklu dosya sistemi (exFAT, Ext2/3/4 journal, APFS, HFS+, ReFS) + bölüm kurtarma
6. **Faz 5** — NVMe SMART + SSD/TRIM farkındalığı + Bayesian arıza tahmini
7. **Faz 6** — VSS snapshot analizi + BitLocker çözme + Unified Timeline + artifact ingest (email/browser/registry/memory)
8. **Faz 7** — Profesyonel UX (ışık teması, önizleme bölmesi, dizin ağacı), gerçek PDF raporlama, vaka yönetimi, paketleme (NSIS installer)

## Geliştirme
```bash
# Tip kontrolü (renderer + main)
npx tsc --noEmit -p tsconfig.web.json
npx tsc --noEmit -p tsconfig.node.json

# Üretim derlemesi (out/ klasörü)
npm run build
```

## Lisans
MIT License
