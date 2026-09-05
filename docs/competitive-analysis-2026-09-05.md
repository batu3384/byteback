# Rekabet analizi — byteback vs piyasa

- Tarih: 2026-09-05
- Karşılaştırılan: R-Studio, UFS Explorer RAID Recovery, DMDE, Disk Drill, EaseUS
- Yöntem: üretici özellik sayfaları + incelemeler; her iddia byteback kodunda doğrulandı (dosya:line kanıtlı)

## Zaten rakip seviyesinde olanlar

| Yetenek | byteback karşılığı |
|---|---|
| NTFS/FAT/exFAT/ext4/HFS+/APFS/ReFS ayrıştırma + silinmiş dosya kurtarma | `native/src/fs/*_parser.cpp`, tam MFT yürüyüşü + orphan `FILE0` geçişi |
| İmza tabanlı carve (PhotoRec sınıfı) | 150+ imza, bounded-emission politikası, BGC (2/3-fragman), MP3/MP4/MKV/OGG/TIFF/ZIP/SQLite yapısal boyutlandırıcılar |
| RAID 0/1/5/6/10 sanal yeniden kurma | `fs/virtual_raid.cpp` + GF(2⁸) RS Q-parite, 2-disk arıza çözümü (Q-syndrome tek eksik blok dahil) |
| BitLocker FVEK/parola açma | `fs/bitlocker_unlock.cpp` (RFC 3610 CCM, Openwall stretch vektörü testli) |
| RAW/E01 imaj + HTTP uzaktan imaj okuma | `io/byte_source.cpp`, `attachEwfImage/attachHttpRawImage/attachRawFile` |
| Önizleme (görsel/PDF/metin/video ilk kare) | `recovery/preview_reader.cpp` + dürüst kod notları |
| Hash zincirli adli denetim günlüğü + runtime doğrulama | `forensic/audit_logger.cpp`, `VerifyAuditChainFile` |
| Hex editör + entropi göstergesi + SMART | `HexEditor.tsx`, `smart_monitor.cpp` |
| Disk imajı (RAW/E01) + MD5 | `imager/disk_imager.cpp`, `ewf_writer.cpp` |
| VSS snapshot + USN + LogFile doğrulamalı silme kanıtı | `vss_scanner.cpp`, `usn.cpp`, `ntfs_logfile.cpp` |

## Bu turda kapatılan eksikler

1. **Klasör ağacı koruyarak kurtarma** (Disk Drill/DMDE standardı) — `safeRelativeDir` + `preservePaths`
2. **Kayıp bölüm taraması** (TestDisk/DMDE standardı) — `scanForPartitions` UI'a bağlandı
3. **Özel imza overlay** — `setSignatureOverlay` + Ayarlar'dan yüklenebilir JSON
4. **Boyut/tarih süzgeçleri** — SQL'e parametrelendirilmiş eklendi, ResultsView girişleri
5. **quick_check rc yanlışlaması** — SQLITE_ABORT beklendiği gibi işleniyor

## R-Studio/UFS'e karşı hâlâ açık (önceliklendirilmiş yol haritası)

| # | Eksik | Rakip karşılığı | Önerilen yaklaşım | Efor |
|---|---|---|---|---|
| 1 | XFS / Btrfs / ZFS / F2FS / UFS ayrıştırıcıları | UFS Explorer tam destek | Önce XFS (basit agelilk superblock), sonra Btrfs; ZFS bileşik — uzman sürüm sonrası | Büyük |
| 2 | RAID parametre oto-tespiti (stripe size/offset/rotation) | R-Studio "otomatik RAID" | Üye disklerde entropy/parite-korelasyon taraması; mevcut `reconstructRaid` manuel | Orta |
| 3 | VMDK/VHD/VHDX/VDI imaj açma | UFS/R-Studio | `ByteSource` arayüzüne VHD/VHDX footer+BAT okuyucu; sparse destekli | Orta |
| 4 | E01 sıkıştırma + bad-sector haritası | R-Studio/Standard E01 | ewf_writer'a zlib (sığ: yalnız uncompressed bugünkü) + error section | Orta |
| 5 | İmaj devam ettirme + bad-sector skip haritası | R-Studio imaj resume | disk_imager'a offset checkpoint (scans tablosuna benzer) | Küçük-orta |
| 6 | İçerik-hash dedup (taramada MD5) | DMDE/R-Studio dedup | Carve kayıtlarında ilk-64KB MD5 + files tablosuna hash kolonu | Orta |
| 7 | Hex editörde disk-geneli hex/metin arama + imlek işaretleri | R-Studio hex editör | readSectors yürüyüşü + sonuç listesi | Küçük |
| 8 | NTFS MFT kayıt görüntüleyici (id→kayıt, attribute list) | R-Studio/DMDE | getMftRecordById bridge + şablon UI | Orta |
| 9 | Ses önizleme | Disk Drill | `<audio>` için WAV proxi üretimi (ffmpeg opsiyonel) | Küçük |
| 10 | Dosya onarım modülleri (JPEG/PDF header rebuild) | Stellar/JPEG repair | Yarım JPEG EOI ekleme, PDF xref rebuild — kurtarma sonrası opsiyonel adım | Orta |
| 11 | Ağ üzerinden kurtarma | R-Studio network | Electron zaten TCP konuşur; agent/istemci modeli | Büyük |
| 12 | ext3 journal replay + ext4 dealloc? | UFS | jbd2 log yürüyüşü | Orta |

## Bilinçli sınırlar (rekabet öncesi tavanlar, belgeli)

- LZNT1: gerçek Windows sıkıştırmalı blob fixture yok (yalnız el yapımı akış)
- ZFS: bileşik pools+oremap — uzman sürüm kapsamı
- Kagıt holder: cache/dedup — sınırlı kaynakla OK

## Sonuç

Temel kurtarma yetenekleri (NTFS MFT, carve, RAID6, BitLocker, E01, VSS, USN, denetim zinciri) rakip seviyede ve çoğu yerde daha doğrulanmış (363 native test, RFC/standart vektörleri). Açık kalan 12 kalem öncelik sırasına göre yol haritasına işlendi; 1-4. kalemler "profesyonel sürüm" iddiası için kritik.
