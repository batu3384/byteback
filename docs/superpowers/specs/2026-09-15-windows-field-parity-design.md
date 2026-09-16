# Program 1 — Windows saha paritesi (tasarım)

**Tarih:** 2026-09-15  
**Durum:** Onaylı kapsam; golden-then-field  
**Kod adı:** Disk Drill USB-foto işi (satır B) + R-Studio Extra Found INDX dilimi (Windows)

## Hedef

Windows NTFS / FAT32 / exFAT’te silinmiş foto ve klasör adını lab’de MD5 ile kanıtla; saha kapısı USB satır B. Mac native yok. Disk Drill Mac paritesi iddiası yok. APFS yalnız imaj/.E01.

## Neden bu tavan

Byteback Windows C++ examiner (`native/src/io/disk_reader_win.cpp`; POSIX `openDrive` yok). Disk Drill Mac sihirbazı ve R-Studio Extra Found + ağ ajanı + WinPE kopyalanmaz. BitLocker TPM-only bypass, sessiz yalan tarama, ağ ajanı P1 dışı.

Kalan programlar (bu spec’te yok): 2 silinmiş-durup Linux/Mac metadata (jbd2, APFS snapshot), 3 format (VHD/Btrfs), 4 inceleyici (hex arama, MFT viewer), 5 üretim, 6 tüketici sihirbazı.

## Mevcut (lab, saha değil)

- FAT 0xE5 + silinmiş zincir ipucu (`applyFatDeletedChainHint`) — zincir boş/kısa ise koşu doldurmaz
- Canlı FAT golden: `GoldenRecoveryTest.FatQuickFindAndRecover` — “Hello FAT16”, silinmiş 5 JPEG değil
- NTFS `$I30` yalnız MFT dizin `$INDEX_ALLOCATION` run’ları; serbest küme INDX yok
- `OrphanIndxMagicDoesNotEmitI30` ham `INDX` magic’i bilerek yutar
- Unallocated carve, HEIC/CR2, önizleme, preservePaths (bridge), audit, TRIM, E01

## P1 kapısı (ölçüt)

Lab (ajan, sentetik):

1. FAT32: 5 silinmiş JPEG, FAT kümeleri serbest (0), veri duruyor → quick+deep 5/5 kurtarma, kaynak JPEG ile byte-exact MD5.
2. exFAT: silinmiş dosya alt dizinde → `safeRelativeDir(path)` + `joinDestDir` hedefte klasör ağacı; MD5 geçer.
3. NTFS: geçerli USA + FILE_NAME INDX serbest kümede, MFT `$I30` run’ında değil → isim `ntfs_i30_unalloc`; ham 4 bayt `INDX` false-positive yok (`OrphanIndxMagicDoesNotEmitI30` yeşil).
4. Kurtarma yazımı: UTF-8 hedef (`CreateFileW` / `std::filesystem::u8path`); ACP-dışı karakterli klasörde resident dosya açılır.

Saha (kullanıcı medyası; ajan prosedür + kayıt, PASS uydurulmaz):

- **B önce:** FAT32 USB 5 silinmiş foto, 5/5 MD5. Geçmezse motor triyaj; “Disk Drill paritesi” cümlesi yok.
- **C:** exFAT SD, preservePaths.
- **A:** NTFS HDD, quick+deep+carve (bilinen tavan: ≤500GB / ≤500K dosya).

## Bilinçli tavan (P1)

- Mac app, APFS snapshot, Time Machine — yok; README “Disk Drill Mac paritesi” yok
- Ağ ajanı, WinPE, BitLocker TPM-only bypass — yok
- jbd2, Btrfs, VHD, H.264 decode — sonraki program
- Unalloc INDX isim + MFT runs yoksa: keşif kaydı veya düşük conf; veri yoksa carve. Adli: isim ≠ byte-exact dosya
- `$INDEX_BITMAP` skip-filter değil
- Recarve yalnız `parseIndxRecord` + USA uygulanmış + en az bir FILE_NAME; ham `INDX` değil
- `$Bitmap` yoksa unalloc INDX: tavan 65536 küme (ponytail); tam HDD linear tarama yok

## Yaklaşım

Golden-then-field. Lab yalan söylemez. Saha hâlâ kullanıcının USB’si.

Akış: ScanCoordinator → FAT/exFAT 0xE5 + NTFS MFT `$I30` + unalloc INDX recarve + unalloc carve → RecoveryEngine MD5 → saha B.

## Başarı

- Lab: 5 silinmiş JPEG MD5; silinmiş NTFS klasör/dosya adı unalloc INDX’ten; ham INDX FP yok
- Saha B: 5/5 byte-exact (kullanıcı medyası, `docs/field-test-2026-09/row-b/`)
- Disk Drill USB-foto işinde kanıtlı denk/üstün **yalnız Windows**; Mac’de iddia yok
- R-Studio Extra Found’un **Windows NTFS INDX** diliminde kapanış; journal/APFS değil
