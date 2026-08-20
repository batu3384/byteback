# Byteback â€” Mimari

Bu belge, kod tabanÄ±ndaki modÃ¼l haritasÄ±nÄ± ve veri akÄ±ÅŸÄ±nÄ± Ã¶zetler. Hedef
kitle: projeye katkÄ± yapacak mÃ¼hendisler. KullanÄ±cÄ± yÃ¼zÃ¼ iÃ§in `README.md`'ye bakÄ±n.

## Katmanlar ve Veri AkÄ±ÅŸÄ±

```
Renderer (React)          Main (Electron)           Native (C++ .node)
â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€          â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€           â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
components/*  â”€â”€preloadâ”€â”€â–¶ ipc-handlers.ts â”€â”€â”€â”€â”€â”€â”€â”€â–¶ bridge_*.cpp
window.api                 native-bridge.ts          â”‚
                                                  â”Œâ”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
                                                  â”‚ byteback::Engine     â”‚
                                                  â”‚ ScanCoordinator  â”‚
                                                  â”‚ DiskImager/Ewf   â”‚
                                                  â”‚ RecoveryEngine   â”‚
                                                  â”‚ VirtualRaid      â”‚
                                                  â””â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                                                     â”‚ DiskReader (DeviceIoControl)
                                                  Fiziksel disk (salt-okunur)
```

- Renderer hiÃ§bir zaman native'e doÄŸrudan dokunmaz; `preload/index.ts`
  `contextBridge` ile beyaz liste API'si aÃ§ar (`contextIsolation: true`).
- TÃ¼m disk eriÅŸimi `GENERIC_READ` Ã¼zerindendir; motor yazma yapmaz (imaj
  Ã§Ä±ktÄ±sÄ± dosyaya, diske deÄŸil).

## Native ModÃ¼ller (`native/src/`)

| Dizin | Sorumluluk | Ã–ne Ã§Ä±kan dosyalar |
|---|---|---|
| `io/` | Ham fiziksel disk eriÅŸimi, kÃ¶tÃ¼ sektÃ¶r telemetrisi | `disk_reader_win.cpp` |
| `fs/` | Dosya sistemi ayrÄ±ÅŸtÄ±rÄ±cÄ±larÄ± ve yerleÅŸim matematiÄŸi | `ntfs_parser.cpp`, `fat_parser.cpp`, `ext4_parser.cpp`, `fat_chain.cpp` (saf FAT zincir/DOS-zaman), `raid_layout.cpp` (saf RAID 5/6/10 yerleÅŸimi), `raid6_math.cpp` (GF(2â¸)), `virtual_raid.cpp`, `partition_scanner.cpp` |
| `carver/` | Ä°mza tabanlÄ± kurtarma | `signature_engine.cpp` (Aho-Corasick + FOV + BGC kurtarma yolu), `bgc.cpp` (iki parÃ§alÄ± gap carving) |
| `recovery/` | DosyayÄ± diskten hedefe yazma | `recovery_engine.cpp` (sparse/LZNT1 aÃ§ma, MD5) |
| `imager/` | Disk imajlama | `disk_imager.cpp` (raw/EWF seÃ§imi), `ewf_writer.cpp` (E01 konteyneri) |
| `crypto/` | Ã–zet + AES-128/256-XTS (FVEK) + AES-CCM + SHA-256 | `md5.cpp`, `aes_xts.cpp`, `aes_ccm.cpp`, `sha256.cpp` |
| `db/` | SQLite Ã¼stveri deposu | `metadata_store.cpp` (taramalar/dosyalar), `metadata_store_case.cpp`, `metadata_store_content.cpp` |
| `forensic/` | Denetim gÃ¼nlÃ¼ÄŸÃ¼ + NSRL MD5 seti | `audit_logger.cpp` (SHA-256, RFC 6234), `nsrl_lookup.cpp` |
| `smart/` | ATA SMART + NVMe saÄŸlÄ±k gÃ¼nlÃ¼ÄŸÃ¼ | `smart_monitor.cpp` |
| `bridge/` | NAPI baÄŸlayÄ±cÄ±larÄ±, konuya gÃ¶re bÃ¶lÃ¼nmÃ¼ÅŸ | `bridge_common.h`, `bridge_{drives,scan,imager,wipe}.cpp`, `napi_bridge.cpp` (yalnÄ±zca Init) |
| `security/` | Dosya/boÅŸ alan imhasÄ±; PhysicalDrive seri kapÄ±sÄ± | `data_shredder.cpp` |

## DoÄŸruluk GÃ¼vencesi

Kritik matematiÄŸin tamamÄ± birim testlidir (`native/tests/`; bu makinede
`ctest -C Release` 194 geÃ§ti, `Ewf.OptionalEwfinfoCrossCheck` skip):

- **MD5 / SHA-256** â€” RFC 1321 / RFC 6234 vektÃ¶rleri, zincir katlama testi.
- **GF(2â¸) Reed-Solomon** â€” alan aksiyomlarÄ±, Ã¼s tablosu, iki-disk kaybÄ±
  kurtarma cebiri (polinom 0x11D).
- **RAID yerleÅŸimi** â€” stripeâ†’(veri, parite) beklenti tablolarÄ± ve her
  stripe'ta her diskin birer kez kullanÄ±ldÄ±ÄŸÄ± permutasyon deÄŸiÅŸmezgeci.
- **FAT zincir + DOS zaman** â€” sentetik tablolar, dÃ¶ngÃ¼ korumasÄ±, artÄ±k gÃ¼n
  vektÃ¶rleri (Python `datetime` ile baÄŸÄ±msÄ±z doÄŸrulanmÄ±ÅŸ).
- **LZNT1 / USA fixup / USN / entropi / EWF konteyner** â€” kendi format
  ayrÄ±ÅŸtÄ±rÄ±cÄ±larÄ±yla gidiÅŸ-dÃ¶nÃ¼ÅŸ testleri.

Sahne arkasÄ±nda kalan spek sÄ±nÄ±rlarÄ± (BitLocker ÅŸifre kÄ±rma yok â€” FVEK hex
varsa XTS decrypt, GPU PFAC yok, APFS recursive omap btree (tam snapshot deÄŸil), hÄ±zlÄ± NTFS `$MFT` run yÃ¼rÃ¼yÃ¼ÅŸÃ¼) READMEâ€™de yazÄ±lÄ±dÄ±r.

## Ã‡alÄ±ÅŸma ZamanÄ± VarlÄ±klarÄ±

- `resources/signatures.json` â€” isteÄŸe baÄŸlÄ± kullanÄ±cÄ± imza seti; yoksa motor
  gÃ¶mÃ¼lÃ¼ ~114 imzalÄ± tabloyu kullanÄ±r.
- `<userData>/byteback.db` â€” tarama Ã¼stverisi (WAL).
- `<userData>/byteback.db.audit.log` â€” SHA-256 hash zincirli denetim
  gÃ¼nlÃ¼ÄŸÃ¼; raporlar son kayÄ±tlarÄ± gÃ¶mer.

## GeliÅŸtirme KomutlarÄ±

```bash
npm run build:native   # C++ motoru derle (cmake-js)
npm run dev            # native + electron-vite geliÅŸtirme oturumu
npm run typecheck      # tsc (web + node)
npm run test           # Vitest (renderer)
npm run test:native    # GoogleTest via ctest (sayÄ± ctest Ã§Ä±ktÄ±sÄ±na baÄŸlÄ±)
npm run test:e2e       # Playwright Electron duman (Ã¶nce `npm run build`)
npm run dist           # NSIS kurulum paketi (release/)
```

Not: `npm run build:native` test Ã¼retecinin Ã¶nbelleÄŸini sÄ±fÄ±rlar; test
koÅŸusundan Ã¶nce `cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON` ile
yeniden yapÄ±landÄ±rÄ±n.
