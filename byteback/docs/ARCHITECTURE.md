# Byteback — Mimari

Bu belge, kod tabanındaki modül haritasını ve veri akışını özetler. Hedef
kitle: projeye katkı yapacak mühendisler. Kullanıcı yüzü için `README.md`'ye bakın.

## Katmanlar ve Veri Akışı

```
Renderer (React)          Main (Electron)           Native (C++ .node)
────────────────          ───────────────           ──────────────────
components/*  ──preload──▶ ipc-handlers.ts ────────▶ bridge_*.cpp
window.api                 native-bridge.ts          │
                                                  ┌──┴───────────────┐
                                                  │ byteback::Engine     │
                                                  │ ScanCoordinator  │
                                                  │ DiskImager/Ewf   │
                                                  │ RecoveryEngine   │
                                                  │ VirtualRaid      │
                                                  └──┬───────────────┘
                                                     │ DiskReader (DeviceIoControl)
                                                  Fiziksel disk (salt-okunur)
```

- Renderer hiçbir zaman native'e doğrudan dokunmaz; `preload/index.ts`
  `contextBridge` ile beyaz liste API'si açar (`contextIsolation: true`).
- Tüm disk erişimi `GENERIC_READ` üzerindendir; motor yazma yapmaz (imaj
  çıktısı dosyaya, diske değil).

## Native Modüller (`native/src/`)

| Dizin | Sorumluluk | Öne çıkan dosyalar |
|---|---|---|
| `io/` | Ham fiziksel disk erişimi, kötü sektör telemetrisi | `disk_reader_win.cpp` |
| `fs/` | Dosya sistemi ayrıştırıcıları ve yerleşim matematiği | `ntfs_parser.cpp`, `fat_parser.cpp`, `ext4_parser.cpp`, `fat_chain.cpp` (saf FAT zincir/DOS-zaman), `raid_layout.cpp` (saf RAID 5/6/10 yerleşimi), `raid6_math.cpp` (GF(2⁸)), `virtual_raid.cpp`, `partition_scanner.cpp` |
| `carver/` | İmza tabanlı kurtarma | `signature_engine.cpp` (Aho-Corasick + FOV + BGC kurtarma yolu), `bgc.cpp` (iki parçalı gap carving) |
| `recovery/` | Dosyayı diskten hedefe yazma | `recovery_engine.cpp` (sparse/LZNT1 açma, MD5) |
| `imager/` | Disk imajlama | `disk_imager.cpp` (raw/EWF seçimi), `ewf_writer.cpp` (E01 konteyneri) |
| `crypto/` | Özet + AES-128/256-XTS (FVEK) + AES-CCM + SHA-256 | `md5.cpp`, `aes_xts.cpp`, `aes_ccm.cpp`, `sha256.cpp` |
| `db/` | SQLite üstveri deposu | `metadata_store.cpp` (taramalar/dosyalar), `metadata_store_case.cpp`, `metadata_store_content.cpp` |
| `forensic/` | Denetim günlüğü + NSRL MD5 seti | `audit_logger.cpp` (SHA-256, RFC 6234), `nsrl_lookup.cpp` |
| `smart/` | ATA SMART + NVMe sağlık günlüğü | `smart_monitor.cpp` |
| `bridge/` | NAPI bağlayıcıları, konuya göre bölünmüş | `bridge_common.h`, `bridge_{drives,scan,imager,wipe}.cpp`, `napi_bridge.cpp` (yalnızca Init) |
| `security/` | Dosya/boş alan imhası; PhysicalDrive seri kapısı | `data_shredder.cpp` |

## Doğruluk Güvencesi

Kritik matematiğin tamamı birim testlidir (`native/tests/`; bu makinede
`ctest -C Release` 194 geçti, `Ewf.OptionalEwfinfoCrossCheck` skip):

- **MD5 / SHA-256** — RFC 1321 / RFC 6234 vektörleri, zincir katlama testi.
- **GF(2⁸) Reed-Solomon** — alan aksiyomları, üs tablosu, iki-disk kaybı
  kurtarma cebiri (polinom 0x11D).
- **RAID yerleşimi** — stripe→(veri, parite) beklenti tabloları ve her
  stripe'ta her diskin birer kez kullanıldığı permutasyon değişmezgeci.
- **FAT zincir + DOS zaman** — sentetik tablolar, döngü koruması, artık gün
  vektörleri (Python `datetime` ile bağımsız doğrulanmış).
- **LZNT1 / USA fixup / USN / entropi / EWF konteyner** — kendi format
  ayrıştırıcılarıyla gidiş-dönüş testleri.

Sahne arkasında kalan spek sınırları (BitLocker şifre kırma yok — FVEK hex
varsa XTS decrypt, GPU PFAC yok, APFS recursive omap btree (tam snapshot değil), hızlı NTFS `$MFT` run yürüyüşü) README’de yazılıdır.

## Çalışma Zamanı Varlıkları

- `resources/signatures.json` — isteğe bağlı kullanıcı imza seti; yoksa motor
  gömülü ~114 imzalı tabloyu kullanır.
- `<userData>/byteback.db` — tarama üstverisi (WAL).
- `<userData>/byteback.db.audit.log` — SHA-256 hash zincirli denetim
  günlüğü; raporlar son kayıtları gömer.

## Geliştirme Komutları

```bash
npm run build:native   # C++ motoru derle (cmake-js)
npm run dev            # native + electron-vite geliştirme oturumu
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer)
npm run test:native    # GoogleTest via ctest (sayı ctest çıktısına bağlı)
npm run test:e2e       # Playwright Electron duman (önce `npm run build`)
npm run dist           # NSIS kurulum paketi (release/)
```

Not: `npm run build:native` test üretecinin önbelleğini sıfırlar; test
koşusundan önce `cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON` ile
yeniden yapılandırın.
