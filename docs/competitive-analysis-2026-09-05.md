# Rekabet analizi — byteback vs piyasa

- Tarih: 2026-09-05
- Karşılaştırılan: R-Studio, UFS Explorer RAID Recovery, DMDE, Disk Drill, EaseUS
- Yöntem: üretici özellik sayfaları + incelemeler; her iddia byteback kodunda doğrulandı (dosya:line kanıtlı)

## Zaten kodda olanlar (lab; saha paritesi değil)

| Yetenek | byteback karşılığı |
|---|---|
| NTFS/FAT/exFAT/ext4/XFS/HFS+/APFS/ReFS/ISO9660/UDF ayrıştırma + silinmiş dosya kurtarma | `native/src/fs/*_parser.cpp`, tam MFT yürüyüşü + orphan `FILE0` geçişi; NTFS yedek boot (son sektör) + `$MFTMirr` rec0 `$DATA` run + `$ATTRIBUTE_LIST` uzantı `$DATA`/ADS/`$EA`/`$REPARSE_POINT` (resident/non-resident) + `$Bitmap` rec6 `$MFT` run; FAT yedek boot (`bkBootSec`/6) + volume label (`fat_vol_label`); exFAT yedek boot (sektör 12) + volume label 0x83 (`exfat_vol_label`); ext4 yedek super (4 MiB / 8193×1K) + yedek GDT (grup 1/3/5/7) + `s_volume_name`; XFS AG1 yedek `XFSB` (ilk 1 MiB) + `sb_fname`; HFS+ unused slot + journal katalog (`hfs_journal`, BE/LE, disk yazılmaz) + yedek VH (volume-end-1024) + resource fork (`name.rsrc`, overflow `0xFF`) + volume name (`hfs_vol_name`); APFS yedek NXSB (son 4096) + inode create/mod; ReFS yedek SUPB (son küme + last-1); ISO9660 PVD `CD001` LBA 16 + volume id + recording date + Joliet + Rock Ridge NM + SL + TF + CE + El Torito `BOOT.IMG` (default + 0x90/0x91 section) + multi-extent 0x80; UDF AVDP/FSD + EFE/short_ad/long_ad + yedek N-256 + VAT (tip 248) + metadata partition (tip 250/251) + bitmap orphan (tip 252) + LVD volume id (`udf_vol_id`) + PVD volume id (`udf_pvd_id`); APM `ER`/`PM` (512/2048 B, GPT/MBR yoksa) |
| İmza tabanlı carve (PhotoRec sınıfı) | 150+ imza, bounded-emission politikası, BGC (2/3-fragman), MP3/MP4/MKV/OGG/TIFF/ZIP/SQLite yapısal boyutlandırıcılar |
| RAID 0/1/5/6/10/JBOD/1E sanal yeniden kurma + geometri tespiti | `fs/virtual_raid.cpp` + `fs/raid_detect.cpp` (RAID5 şerit: XOR + FS-imza; `fsConfirmed`; RAID5 mdadm 4 düzen `raid5Algorithm` 0..3; `memberOrder`; mdadm super1 1.2/1.1/1.0 + LINEAR; Intel IMSM MPB; SNIA DDF `0xDE11DE11`; RAID1E near tek N) |
| LDM public volume + Storage Spaces keşif | `fs/ldm_parser.cpp` PRIVHEAD BE + MBR `0x42`; PRT3 + DSK3 GUID bind; spanned JBOD + CMP3 stripe RAID0 + RAID5 assemble; GPT Spaces `SPACEDB ` keşif (slab yok) |
| LVM2 PV + linear/striped/mirror/raid LV | `lvm_parser.cpp` LABELONE CRC; linear; striped RAID0; `type=mirror` RAID1; modern `type=raid` raid1/raid0/raid5_ls/raid6/raid10; ayrı DiskReader/imaj PV |
| BitLocker FVEK/parola + LUKS1 parola | `fs/bitlocker_unlock.cpp`; `fs/luks.cpp` (PBKDF2-HMAC-SHA256, AES-XTS-plain64 128/256, AF splitter; offset 0 değilse MBR/GPT/APM yürüyüşü; LUKS2/Argon2 yok) |
| RAW/E01 imaj + HTTP uzaktan imaj okuma | `io/byte_source.cpp`, `attachEwfImage/attachHttpRawImage/attachRawFile`; E01 zlib `ewf_writer.cpp`/`ewf_reader.cpp`; HTTP sibling `.E02`; split RAW `.001`+`.002` concat |
| İmaj devam ettirme (RAW .part + sidecar) | `imager/disk_imager.cpp` resume |
| İçerik-hash dedup | carve anında + metadata tembel `hashEmptyContent` / `files.content_hash` + grup rozeti |
| Önizleme (görsel/PDF/metin/video ilk kare) | `recovery/preview_reader.cpp` + dürüst kod notları |
| Hash zincirli adli denetim günlüğü + runtime doğrulama | `forensic/audit_logger.cpp`, `VerifyAuditChainFile` |
| Hex editör + disk-geneli hex/metin arama + entropi + SMART | `HexEditor.tsx`, `search/hex_search.cpp`, `smart_monitor.cpp` |
| NTFS MFT kayıt görüntüleyici (keşif) | `fs/mft_record_view.cpp` + Hex panel; Results `show-mft` → `files.mft_ref`; kayıt `$MFT` `$DATA` run (`$MFTMirr` rec0 fallback); unread FILE uydurmaz |
| Sabit/dinamik VHD, VHDX (log replay + relative parent), VMDK/VDI/QCOW2 parent, DMG/UDIF | `io/vhd_source.cpp` + `attachVhdMemory` / `attachVhdFile`; clone bellek paylaşır; bellek-parent (locator/hint/UUID/backing yok) reddedilir; LZFSE/şifreli DMG reddedilir; QCOW2 zlib küme; VMDK zlib grain; UDIF ADC; VHDX log replay; 64 MiB tavan |
| Disk imajı (RAW/E01) + MD5 | `imager/disk_imager.cpp`, `ewf_writer.cpp` |
| VSS snapshot + USN + LogFile doğrulamalı silme kanıtı | `vss_scanner.cpp`, `usn.cpp`, `ntfs_logfile.cpp` |

## Bu turda kapatılan eksikler

1. **Klasör ağacı koruyarak kurtarma** (Disk Drill/DMDE standardı) — `safeRelativeDir` + `preservePaths`
2. **Kayıp bölüm taraması** (TestDisk/DMDE standardı) — `scanForPartitions` UI'a bağlandı
3. **Özel imza overlay** — `setSignatureOverlay` + Ayarlar'dan yüklenebilir JSON
4. **Boyut/tarih süzgeçleri** — SQL'e parametrelendirilmiş eklendi, ResultsView girişleri
5. **quick_check rc yanlışlaması** — SQLITE_ABORT beklendiği gibi işleniyor
6. **Parçalı FAT undelete** — yedek FAT zinciri + imza sınırı (yanlış-byte başarı yok)
7. **Unalloc INDX** — I/O unread + canlı MFT aynı isim filtresi
8. **FAT LFN UTF-8 + silinmiş LFN** — `utf16leToUtf8`, 0xE5 LFN geri kurma
9. **jbd2 isimler** — `ext4_journal` keşif; unread sentinel; inode yoksa false-unread yok
10. **Unalloc INDX Extra Found içerik (JPEG/PNG/PDF/ZIP)** — `ntfs_i30_carve`; canlı `$DATA` çalınmaz; ad-only durur
11. **Ses önizleme (WAV/ID3/Ogg)** — native `audio/*` kind + ResultsPreviewPanel `<audio>`; ffmpeg WAV proxy yok
12. **JPEG EOI onarım** — SOS’lu kesik JPEG’de `FF D9`; sidecar `*_repaired`; dest MD5 değişmez
13. **PDF xref rebuild** — `%%EOF` yoksa sidecar xref/trailer; orijinal dosyaya dokunulmaz
14. **ZIP merkez dizini rebuild** — EOCD yoksa sidecar CD+EOCD; sağlam ZIP’e dokunulmaz
15. **Dinamik VHD (BAT) + VHDX** — FAT16 `TEST.TXT`; bellek-VHD type 4 / bellek-VHDX parent reddedilir; dosya VHD type 4 Parent Unicode Name ve VHDX `relative_win32_path` parent FAT okur; LogGuid + LogLength 0 mount; dirty log `loge`/`desc`/`data`/`zero`; kesik log reddedilir; 64 MiB (`Fat16ListsTestTxt`, `EmptyLogGuidStillMountsFat16`, `ReplaysLogRestoresFat16`, `ReplaysLogZeroClearsSector`, `TruncatedLogDoesNotMount`, `DifferencingChildReadsParentFat16`, `ParentVhdxDoesNotMount`, `DifferencingVhdDoesNotMount`, `VhdSource.DifferencingChildReadsParentFat16`)
16. **VMDK sparse + VDI** — FAT16 `TEST.TXT`; bellek-VMDK parentCID / bellek-VDI type 4 reddedilir; dosya VMDK `parentFileNameHint` ve VDI type 4 UUID (aynı dizin `.vdi`, tavan 32) parent FAT okur; zlib grain (`Fat16ListsTestTxt`, `CompressedVmdkFat16ListsTestTxt`, `ParentVmdkDoesNotMount`, `DifferencingChildReadsParentFat16`, `DifferencingVdiDoesNotMount`, `VdiSource.DifferencingChildReadsParentFat16`)
17. **`attachVhdFile` + clone** — dosyadan bağla; clone bellek hacmini paylaşır, FAT16 `TEST.TXT`
18. **RAID5 şerit FS-imza** — XOR katları bağlanınca MBR/NTFS/FAT/ext birleşik görünüm skoru gerçek şeridi seçer (`FsSignaturePicks64KOverParityTied128K`)
19. **X3F SECd boyut** — `parseX3fBounded`: kuyruk işaretçisi + SECd; FOVb dizinsiz drop
20. **TIFF Make/DNGVersion** — Nikon→`.nef`, Sony→`.arw`, DNGVersion/Adobe→`.dng`
21. **HTTP HEAD 405/501** — `Range: bytes=0-0` + Content-Range toplam
22. **Unicode imaj hedefi** — `utf8Path` DiskImager/EwfWriter; `RawImageWritesUtf8DestDir`
23. **İnceleyici imaj tara** — Dashboard `pick-scan-image` + `drivePath "image"` / `driveIndex -2`; RAW/E01/VHD ailesi; recover/hex/preview `volumePath` dosyayı yeniden bağlar (`RawImagePathFindsFat16TestTxt`)
24. **HFS+ unused catalog slot** — yaprak serbest alanındaki silinmiş katalog kaydı `hfs_catalog_unused` (status=0, conf 40–55); canlı fileID tekrar etmez (`UnusedLeafSlotEmitsDeletedFile`)
25. **XFS v5 CRC32C + iunlinked** — `refsCrc32cSkip4` (xfs_cksum skip-4); uyuşmazlık conf 48 kayıt düşmez; AGI `iunlinked` `xfs_unlinked` (`UnlinkedInodeEmitsDeletedFile`)
26. **APFS checkpoint omap** — `nx_xp_desc_*` NXSB kopyası; canlı omap’te yoksa `apfs_omap_deleted` keşif (`CheckpointOmapEmitsDeletedFile`)
27. **Metadata content-hash** — tembel MD5 (ilk min(64KB,size)); `trySetContentHash` idempotent; ResultsView “İçerik dedup analiz et”; aynı (hash,size) “aynı içerik” rozeti, kayıt düşmez (`ContentHashWriteIsIdempotent`, `HashEmptyResidentGroupsSamePayload`)
28. **jbd2 replay** — committed descriptor+commit zinciri; silinmiş dosya journal kopyasından `ext4_journal_replay` + runs; commit yoksa emit yok (`ReplayCommittedFileHasRunsAndPayload`). Disk yazılmaz.
29. **NTFS 2M akışkan emit** — `tempFiles` yok; in-suite `StreamingScanPeakMemoryBelowBoundAt2M` peak delta <700MB; 200K <150MB. PID-unique test temp (`test_temp_path.h`).
30. **Hex imlek** — `userData/hex-marks.json`; 64 tavan, aynı sürücü+yol+sektör tek kayıt; düşman JSON boş liste (`parseHexMarks`).
31. **Sonuç → MFT kaydı** — `files.mft_ref` persist; NTFS emit self-ref; Results “MFT kaydını göster”; Hex `initialMftRef` (`MftRefRoundTrip`, `EmittedMftRefOpensRecordView`).
32. **Faz 4 gerçek bayt** — FAT16 imaj `startScan(-2)` hex+recover `Hello FAT16`; kesik JPEG seed+`setScanVolumeBinding` recover sidecar `FF D9` (carve footer zorunlu kalır).
33. **Kanıt imajını klonla** — `DiskImager` `attachEvidenceImage`; IPC `driveIndex -2` + dosya `volumePath`; ImagerView tarama-bağlı dosyayı varsayılan kaynak alır (`ImagesEvidenceFileToRawClone`).
34. **RAID üye imajları** — `VirtualRaid::fromEvidencePaths`; RaidBuilder “Üye imaj ekle”; fiziksel+imaj karışımı reddedilir (`AssemblesRaid0FromEvidenceFiles`, e2e RAID1 FAT16 `Hello FAT16`).
35. **RAID geometri tespiti imajlardan** — `detectRaidFromEvidencePaths`; RaidBuilder otomatik tespit imaj üyelerinde `detectRaidImages`; cihaz yolu reddedilir; RAID1 örnekleri şerit-hizalı (`DetectsRaid6FromEvidenceFiles`, `DetectsRaid1FromEvidenceFat16Copies`, e2e RAID1 `raidLevel===1`).
36. **Kanıt imaj kaynağı Unicode yol** — `FileByteSource` / `extractVhdFile` `utf8Path`; RAW+VHD (`AttachEvidenceImageUtf8Path`, `AttachFileUtf8PathListsTestTxt`). E01 aynı FileByteSource.
37. **RAID10 geometri tespiti** — bitişik ayna çiftleri; A,A,B,B RAID5 XOR’u reddedilir; şerit yalnız FS-imza (`DetectsRaid10StripeViaFsSignature`, `Raid10PairsWithoutFsDoNotImpersonateRaid5`, `FsSignaturePicksRaid10_128KNotDivisor64K`). RAID0 hâlâ tahmin edilmez.
38. **RAID üye sırası** — n rotasyon + reverse (n! yok, tavan 16); `memberOrder` NAPI; RaidBuilder `applyRaidMemberOrder` (`RotatedRaid5MembersStillDetect`, `PairSwappedRaid10StillDetects`). Identity-adjacent RAID5 XOR düşer.
39. **RAID5 mdadm 4 düzen** — left/right × asymmetric/symmetric; XOR sol ve sağ parite; FS-imza seçer (`FsSignaturePicksLeftSymmetricNotAsymmetric`, `FsSignaturePicksRightAsymmetric`). `raid5Algorithm` NAPI 0..3.
40. **GPT yedek başlık** — birincil LBA 1 silinmiş/okunamazsa UEFI AlternateLBA (son sektör) + dizi; TestDisk/R-Studio kayıp tablo (`BackupGptUsedWhenPrimaryWiped`, `BackupGptUsedWhenPrimaryUnread`).
41. **mdadm super1** — sihir `0xa92b4efc`, 1.2 @4KiB / 1.1 @0 / 1.0 son-8KiB; additive csum; kernel RAID5 layout 0..3 → `raid5Algorithm`; `dev_roles` üye sırası; XOR’un denemediği `data_offset`; LINEAR `level=-1` chunk 0 → `RaidLevel::JBOD` (`ParseMdadm12ReadsLeftSymmetricLayout`, `ParseMdadm12ReadsLinear`, `DetectsRaid5FromMdadmWhenDataOffsetNotInControllerSet`, `MdadmRolesReorderMembers`, `DetectsRaid5FromMdadm10AtEnd`, `DetectsLinearFromMdadm`).
42. **LVM2 PV keşif** — `LABELONE`+`LVM2 001` @+512; GPT GUID `E6D6D379`; kayıp bölüm `LVM2 PV` (`Lvm2LabelAtPvStart`, `FindsLvm2LabelOne`, `GptLinuxLvmGuid`).
43. **Intel IMSM** — `Intel Raid ISM Cfg Sig. ` anchor `dsize-2×512`; checksum = le32 toplam − alan; RAID1/5; 2. sektör uzantı (`ParseImsmAnchorReadsRaid1`, `DetectsRaid1FromImsmWhenMembersDiverge`, `DetectsRaid5FromImsmExtendedMpb`). RAID5 layout left-asymmetric (mdadm).
44. **SNIA DDF** — sihir BE `0xDE11DE11` son 512; CRC-32 IEEE alan `0xFFFFFFFF`; primary_lba + `vd_config` `0xEEEEEEEE`; RAID0/1/5; RAID5 RLQ 0/2/3 → right-asym / left-asym / left-sym (`ParseDdfAnchorReadsMagic`, `DetectsRaid0FromDdf`, `DetectsRaid1FromDdfWhenMembersDiverge`, `DetectsRaid5FromDdfLeftAsym`, `DetectsRaid5FromDdfRightAsym`). 32MB tarama yok.
45. **LVM2 linear LV** — label CRC `0xF597A6CF`; MDA `contents = "Text Format Volume Group"`; `stripe_count=1` `type=striped`; PE + `start_extent`; coordinator FAT/NTFS/ext; GPT Linux LVM bölüm (`ParseLvm2LabelReadsPeStart`, `ParseLvm2TextLinearLv`, `LinearLvmLvFindsFat16TestTxt`, `GptLinuxLvmPartitionFindsFat16TestTxt`).
46. **LVM2 striped LV** — `stripe_count` 2..8 + `stripe_size`; PV uuid eşlemesi; VirtualRaid RAID0 per-member offset (`Raid0HonorsPerMemberDataOffsets`, `ParseLvm2TextStripedTwoColumns`, `StripedLvmLvFindsFat16TestTxt`).
47. **LVM2 mirror LV** — `type = "mirror"` `mirror_count` 2..8 `mirrors = [`; VirtualRaid RAID1 `block_size=0` per-member offset; coordinator linear’ı atlar (`ParseLvm2TextMirrorTwoLegs`, `MirrorLvmLvFindsFat16TestTxt`).
48. **LVM2 ayrı-cihaz PV** — `assembleLvmFromOpenDisks` / `assembleLvmFromEvidencePaths` (2..16); GPT Linux LVM veya LABELONE @0; NAPI `assembleLvm`/`assembleLvmImages`; RaidBuilder “LVM PV birleştir”; cihaz yolu reddedilir (`AssemblesStripedLvFromTwoSeparatePvReaders`, `AssemblesStripedLvFromTwoEvidenceFiles`, `AssembleLvmFromEvidencePathsRejectsDevice`).
49. **SNIA DDF RAID6/10** — `prl` 6 / `0x11`; 4 üye (`DetectsRaid6FromDdf`, `DetectsRaid10FromDdf`).
50. **LVM2 `type=raid`** — modern metadata `raid_type=raid1|raid0|raid5_ls` (`raid5`/`raid5_la`/`raid5_ra`/`raid5_rs`); raid1 RAID1; raid0 RAID0; raid5 VirtualRaid RAID5 left-sym default (`ParseLvm2TextRaidTypeRaid1`, `ParseLvm2TextRaidTypeRaid0`, `ParseLvm2TextRaidTypeRaid5Ls`, `RaidTypeRaid1LvmLvFindsFat16TestTxt`, `AssemblesRaid5LsLvFromThreePvReaders`).
51. **LVM2 `type=raid` raid6/raid10** — `raid6`/`raid6_zr`/`raid6_nr`/`raid6_nc` → VirtualRaid RAID6 (4..8); `raid10`/`raid10_near` çift üye 4..8 → RAID10 ayna-çift; 4 PV FAT16 `TEST.TXT` (`ParseLvm2TextRaidTypeRaid6`, `ParseLvm2TextRaidTypeRaid10`, `AssemblesRaid6LvFromFourPvReaders`, `AssemblesRaid10LvFromFourPvReaders`).
52. **ISO 9660 + Joliet + Rock Ridge** — PVD `CD001` logical sector 16 (2048 B); kök dizin; `HELLO.TXT;1` → `HELLO.TXT`; SVD `%/@` `%/C` `%/E` UCS-2 ad tercih; SUSP `NM` POSIX ad; multi-extent bit 7 (0x80) DataRun `byteCount` (`ProbeFindsCd001AtSector16`, `ListsHelloTxt`, `QuickScanFindsHelloTxt`, `RecoversHelloTxtBytes`, `RecoversMultiExtentHelloTxtBytes`, `RecoversMultiExtentOver1MibUnaligned`, `PrefersJolietName`, `PrefersRockRidgeName`).
53. **Split RAW** — `openFileByteSource` `.000`/`.001` sonrası ardışık `.002`… (tavan 256); sınır okuma; yalnız `.001` tek dosya; `.002` yoksa `.003` atlanmaz (`Concatenates001And002AcrossBoundary`, `Lone001IsSingleFile`, `GapWithout002DoesNotSkipTo003`, `EvidenceImageScanFindsFatAcrossSplit`).
54. **UDF** — VRS `BEA01`+`NSR02`/`NSR03`; AVDP sektör 256 veya yedek `N-256`; PD+LVD+FSD; File Entry + EFE 266; AD_IN_ICB / short_ad / long_ad; VAT ICB son sektör (tip 248, `0xFFFFFFFF` kimlik); type 2 `*UDF Metadata Partition` (FE tip 250, short_ad); mirror tip 251; bitmap tip 252 orphan FE (`HELLO.TXT` list+kurtarma + `orphan udf`) (`ProbeFindsBea01AtSector16`, `ListsHelloTxt`, `QuickScanFindsHelloTxt`, `RecoversHelloTxtBytes`, `RecoversEfeAdInIcbBytes`, `RecoversShortAdExtentBytes`, `RecoversLongAdExtentBytes`, `RecoversFromBackupAvdp`, `RecoversHelloTxtViaVat`, `RecoversHelloTxtViaMetadataPartition`, `RecoversHelloTxtViaMetadataMirror`, `RecoversOrphanViaMetadataBitmap`).
55. **El Torito** — ISO Boot Record `EL TORITO SPECIFICATION`; katalog 0x55AA + checksum 0; default 0x88 + extra section 0x90/0x91 + 0x88; `BOOT.IMG` 512×sector_count (`RecoversElToritoBootImg`, `RecoversElToritoSectionBootImg`).
56. **ISO Rock Ridge NM** — dizin kaydı System Use `NM`; Joliet yokken POSIX ad (`PrefersRockRidgeName`).
57. **QCOW2** — `QFI\xfb` v2/v3; L1/L2 uncompressed veya zlib (bit 62, `inflateInit2 -12`); bellek-backing reddedilir; dosya çocuğu backing adı (path yok) parent FAT okur; crypt/snapshot reddedilir; bozuk sıkıştırılmış küme mount etmez; `attachEvidenceImage` `.qcow2`/`.qcow`; 64 MiB tavan (`Fat16ListsTestTxt`, `BackingFileDoesNotMount`, `BackingChildReadsParentFat16`, `CompressedClusterDoesNotMount`, `CompressedClusterListsTestTxt`, `AttachEvidenceImageListsTestTxt`).
58. **LUKS1 parola** — sihir `LUKS\xba\xbe` v1; PBKDF2-HMAC-SHA256 (RFC 4231 HMAC); AES-XTS-plain64 128 **ve 256** (`keyBytes` 32/64); cryptsetup AF; yanlış parola açmaz; payload FAT16 `TEST.TXT`; offset 0 değilse MBR/GPT/APM bölüm yürüyüşü; coordinator `luks_detect` + unlock sonrası iç FS; NAPI `setLuksPassword` (imaj `driveIndex -2`); LUKS2/Argon2 yok (`Rfc4231Case1`, `PasswordSaltIter1Dk32`, `ProbeFindsLuksMagic`, `UnlockPasswordListsFat16TestTxt`, `UnlockPasswordAes256XtsListsFat16TestTxt`, `UnlockPasswordOnMbrPartition`, `UnlockPasswordOnGptPartition`, `UnlockPasswordOnApmPartition`, `WrongPasswordDoesNotUnlock`, `LuksDetectsHeader`, `LuksUnlockQuickScanFindsTestTxt`).
59. **Apple DMG/UDIF** — `koly` footer v4; XML `blkx` + `mish`; RAW `0x00000001` + ZERO `0x00000002` + ADC `0x80000004` + ZLIB `0x80000005`; LZFSE/şifreli reddedilir; `attachEvidenceImage` `.dmg`; 64 MiB tavan (`UdrwFat16ListsTestTxt`, `UdzoFat16ListsTestTxt`, `AdcFat16ListsTestTxt`, `EncryptedDoesNotMount`, `LzfseDoesNotMount`, `AttachEvidenceImageListsTestTxt`).
60. **Apple Partition Map** — sürücü tanımı `ER`; girdi `PM`; `pmMapBlkCnt` 2..64; 512 veya 2048 B blok (okuyucu sektörüne ölçek); `Apple_partition_map`/`Apple_Free`/`Apple_Driver*` atlanır; GPT/MBR yoksa `parseTables`; FAT16 `TEST.TXT` (`ParseApmReadsHfsStart`, `ParseApm2048ReadsHfsStart`, `QuickScanFindsFatOnApmPartition`, `QuickScanFindsFatOnApm2048Partition`).
61. **ISO multi-extent** — dizin kaydı File Flags bit 7 (0x80) + son extent; `HELLO.TXT` iki LBA; >1 MiB hizasız ilk extent DataRun `byteCount` (`RecoversMultiExtentHelloTxtBytes`, `RecoversMultiExtentOver1MibUnaligned`, `ByteCountRoundTrip`).
62. **UDF metadata partition** — LVD type 2 map `*UDF Metadata Partition`; FE tip 250 short_ad; birincil FE silinince mirror LBN + tip 251; bitmap tip 252 allocated LBN, dizin dışı FE `udf_meta_orphan` (`RecoversHelloTxtViaMetadataPartition`, `RecoversHelloTxtViaMetadataMirror`, `RecoversOrphanViaMetadataBitmap`).
63. **mdadm LINEAR / JBOD** — concatenates member payloads (Disk Drill JBOD); şerit yok; `VirtualRaid` `RaidLevel::JBOD`; NAPI 5; RaidBuilder “JBOD / LINEAR” (`ParseMdadm12ReadsLinear`, `DetectsLinearFromMdadm`, `JbodConcatenatesFat16ListsTestTxt`). RAID0 hâlâ XOR’dan tahmin edilmez.
64. **Windows LDM / Dynamic Disk** — PRIVHEAD `PRIVHEAD` BE v2.11/2.12 @ LBA 6 (yedek son sektör) + disk GUID `@0x30`; public start/size; MBR tip `0x42`; GPT `0xAF9B60A0` / `0x5808C8AA`; VBLK PRT3 `start`+`size`+`volume_offset`+`parent`+`diskId`; DSK3 `0x34` / DSK4 `0x44` objid→GUID (replicated VBLK doğru üyeye); CMP3 `0x32` stripe `type=1` RAID0 + `type=3` RAID5 (flag `0x10`, `chunkSectors`); spanned aynı-disk concat (JBOD window + run remap); spanned/striped/RAID5 çok-disk `assembleLdmFromOpenDisks` / `assembleLdmFromEvidencePaths` (NAPI `assembleLdm`/`assembleLdmImages`; RaidBuilder “LDM spanned birleştir”) (`ParsePrivheadReadsPublicStart`, `BackupPrivheadAtLastSector`, `ParsePrt3ReadsStartSize`, `ParseCmp3ReadsStripeChunk`, `MbrType42IsLdm`, `GptTypeIsLdmData`, `QuickScanFindsFatOnLdmPublicRegion`, `QuickScanFindsFatOnLdmWhenBoundsSelectMbr42`, `QuickScanFindsFatOnLdmPrt3Offset`, `QuickScanRecoversFatOnLdmSpannedPrt3`, `AssemblesSpannedFromTwoDisksRecoversFat16`, `AssemblesSpannedFromTwoEvidenceFiles`, `AssembleLdmFromEvidencePathsRejectsDevice`, `AssemblesReplicatedVblkBindsByDsk3Guid`, `AssemblesReplicatedVblkBindsByDsk4Guid`, `AssemblesStripedFromTwoDisksRecoversFat16`, `AssemblesRaid5FromThreeDisksRecoversFat16`).
65. **Windows Storage Spaces** — GPT GUID `0xE75CAF8F`; `SPACEDB ` keşif; `spaces_detect` (slab/column birleştirme yok) (`LooksLikeSpacedb`, `SpacesDetectsHeader`).
66. **RAID1E** — 2-copy near `slot=D*2+copy`; N>=3 tek; kapasite `N/2`; NAPI 6; detect yalnız tek N + FS imza (`NearThreeDiskTable`, `Raid1eListsFat16TestTxt`, `Raid1eSurvivesOneFailedMember`, `DetectsRaid1eViaFsSignature`).
67. **HTTP çok-segment EWF** — `ewfSegmentPath` `.E01`→`.E02` (http URL dahil); `openHttpByteSource` kardeş; SSRF host kuralı aynı (`HttpSiblingSegmentPathIsE02`).
68. **HFS+ journal** — VH `kHFSVolumeJournaledMask` + JournalInfoBlock in-FS; `JNLX` header BE/LE; committed `block_list` katalog yaprağı `hfs_journal` (status=0, conf 50, extent varsa kurtarılabilir); boş txn / bozuk checksum / canlı fileID uydurmaz; dairesel wrap; disk yazılmaz (`JournalCatalogLeafEmitsDeletedFile`, `JournalLittleEndianCatalogLeafEmitsDeletedFile`, `JournalWrapAroundCatalogLeafEmitsDeletedFile`, `EmptyJournalDoesNotInventFiles`, `JournalDoesNotDuplicateLiveFileId`, `BadJournalChecksumDoesNotEmit`).
69. **HFS+ yedek volume header** — birincil `+1024` silinince TN1150 kopya `volume-end-1024`; katalog yürüyüşü aynı (`BackupVolumeHeaderUsedWhenPrimaryWiped`).
70. **NTFS yedek boot** — birincil OEM silinince son sektör kopyası `$MFT` LCN; parser + MFT view + unalloc map + `$LogFile` aynı `loadNtfsBoot` (`BackupBootUsedWhenPrimaryWiped`).
71. **FAT yedek boot** — birincil BPB `bytesPerSector`/`spc` silinince `bkBootSec` (yoksa sektör 6) kopyası kök dizini (`BackupBootUsedWhenPrimaryBpbWiped`).
72. **ext4 yedek superblock** — 4 MiB spray kaçırınca 4 MiB / `8193*1024` / `32768*4096`; kopya `sb_buffer` (dangling spray yok) (`BackupSuperblockOutside4MibSpray`).
73. **XFS AG1 yedek superblock** — AG0 `XFSB` silinince ilk 1 MiB’de geçerli kopya (`BackupAg1SuperblockUsedWhenPrimaryWiped`); birincil sağlamken AG1 uyuşmazlığı hâlâ red.
74. **exFAT yedek boot** — birincil OEM silinince sektör 12 (Backup Boot Region) kök dizini (`ExFatBackupBootUsedWhenPrimaryOemWiped`).
75. **NTFS `$MFTMirr`** — birincil rec0 `FILE` silinince boot `0x38` LCN kopyası `$MFT` `$DATA` run; 1024-kayıt bitişik fallback dışı (`MftMirrUsedWhenPrimaryRec0Wiped`).
76. **ReFS yedek SUPB** — küme 30 sihir silinince son küme kopyası listing (`BackupSupbUsedWhenPrimaryWiped`); unread küme 30 sentinel durur.
77. **NTFS `$ATTRIBUTE_LIST`** — taban kayıtta unnamed `$DATA` yoksa resident **veya** non-resident list uzantı kaydından payload (`AttributeListPullsDataFromExtensionRecord`, `NonResidentAttributeListPullsDataFromExtensionRecord`); named ADS uzantıdan `ntfs_ads` (`AttributeListPullsNamedAdsFromExtensionRecord`); uzantı kayıt ayrı dosya emit etmez.
78. **APFS yedek NXSB** — birincil sihir silinince son 4096 kopyası container+volume (`BackupNxsbUsedWhenPrimaryWiped`); unread birincil sentinel durur.
79. **NTFS `$Bitmap`** — rec6 `$MFT` `$DATA` run üzerinden (boot LCN+6 değil); `$MFTMirr` rec0 fallback (`NtfsBitmapFollowsMftDataRuns`).
80. **NTFS MFT view `$MFT` run** — Hex `getMftRecordView` recN boot LCN+N değil; rec0 `$DATA` run (`$MFTMirr` rec0 fallback) (`FollowsMftDataRunsForNonzeroRef`).
81. **HFS+ resource fork** — katalog `HFSPlusForkData` @172; `name.rsrc` kardeş + overflow tip `0xFF` (`ResourceForkEmittedAsSibling`).
82. **ext4 yedek GDT** — grup 0 `bg_inode_table` 0 ise grup 1/3/5/7 yedek süper sonrası GDT (`BackupGdtUsedWhenPrimaryWiped`).
83. **NTFS `$REPARSE_POINT`** — resident symlink `0xA000000C` / mount `0xA0000003` print name `ntfs_reparse` (`ReparsePointEmitsSymlinkPrintName`).
84. **NTFS `$EA`** — resident **veya** non-resident `FILE_FULL_EA_INFORMATION` `name:ea:…` `ntfs_ea` (`ResidentEaEmitsNamedStream`, `NonResidentEaEmitsNamedStream`).
85. **NTFS EFS** — `$STANDARD_INFORMATION` `FILE_ATTRIBUTE_ENCRYPTED` + `$DATA` flag `0x4000` → `ntfs_efs` keşif (anahtarsız kurtarma yok) (`EncryptedStandardInfoIsEfsNotPlaintext`).
86. **ISO Rock Ridge SL** — SUSP `SL` bileşen yolu resident `iso9660_rr_sl` (`RockRidgeSlEmitsSymlinkTarget`).
87. **FAT volume label** — dirent `ATTR_VOLUME` `0x08` 11-byte etiket (8.3 split yok) `fat_vol_label` keşif; recoverable dosya değil (`VolumeLabelDirentIsDiscoveryNotFile`).
88. **NTFS `$OBJECT_ID`** — resident 16-byte GUID Windows dizgisi `name:objectid` `ntfs_object_id` keşif (`ObjectIdEmitsGuidDiscovery`).
89. **NTFS `$VOLUME_NAME`** — resident UTF-16 etiket `ntfs_vol_name` keşif (`VolumeNameEmitsLabelDiscovery`).
90. **ISO PVD volume id** — ECMA-119 `Volume Identifier` @+40 (Joliet UCS-2 tercih) `iso9660_vol_id` (`VolumeIdEmitsDiscovery`).
91. **HFS+ volume name** — katalog kök klasör parent CNID 1 `hfs_vol_name` (`RootFolderNameIsVolumeDiscovery`).
92. **ext4 `s_volume_name`** — süper +120 16-byte etiket `ext4_vol_name` (`VolumeNameFromSuperblock`).
93. **XFS `sb_fname`** — süper +108 12-byte etiket `xfs_vol_name` (`VolumeNameFromSbFname`).
94. **UDF LVD volume id** — Logical Volume Identifier dstring @+84 `udf_vol_id` (`LogicalVolumeIdentifierEmitsDiscovery`).
95. **exFAT volume label** — kök `0x83` UTF-16LE `exfat_vol_label` (`ExFatVolumeLabelIsDiscoveryNotFile`).
96. **HFS+ rsrc overflow** — extents overflow `forkType` `0xFF` `name.rsrc` run’a birleşir (`ResourceForkOverflowExtentsMergedIntoRuns`).
97. **UDF PVD volume id** — Primary Volume Identifier dstring @+24 `udf_pvd_id` (`PrimaryVolumeIdentifierEmitsDiscovery`).
98. **ISO recording date** — dizin kaydı +18 7-byte GMT `modifiedAt` (`RecordingDateSetsModifiedAt`).
99. **ReFS yedek SUPB last-1** — küme 30 ve son küme boşsa last-1 kopyası (`BackupSupbUsedWhenPrimaryAndLastWiped`).
100. **NTFS mount reparse** — `$REPARSE_POINT` `0xA0000003` print name `ntfs_reparse` (`ReparsePointEmitsMountPrintName`).
101. **UDF FSD file set id** — File Set Identifier dstring @+304 `udf_fsd_id` (`FileSetIdentifierEmitsDiscovery`).
102. **ISO Rock Ridge TF** — SUSP `TF` MODIFY 7-byte dizin kaydı tarihini ezer (`RockRidgeTfOverridesRecordingDate`).
103. **UDF FE mtime** — File Entry / EFE Modification Date `modifiedAt` (`FileEntryModificationTimeSetsModifiedAt`).
104. **HFS+ catalog dates** — `createDate`/`contentModDate` 1904 epoch `createdAt`/`modifiedAt` (`CatalogDatesSetModifiedAt`).
105. **ISO Rock Ridge CE** — SUSP continuation `NM` (`RockRidgeCeContinuesName`) + `SL` (`RockRidgeCeContinuesSymlink`) + `TF` (`RockRidgeCeContinuesTf`).
106. **FAT dirent dates** — `wrtDate`/`crtDate` DOS epoch `modifiedAt`/`createdAt` (`DirentDatesSetModifiedAt`).
107. **NTFS non-resident `$REPARSE_POINT`** — run blob print name (`NonResidentReparsePointEmitsSymlinkPrintName`).
108. **XFS `di_mtime`** — inode +40 BE32 `t_sec` FileRecord `modifiedAt` (`DiMtimeSetsHitMtime`).
109. **APFS inode times** — `j_inode_val` create/mod ns → `createdAt`/`modifiedAt` (`InodeCreateTimeSetsCreatedAt`, `InodeModTimeSetsModifiedAt`).
110. **NTFS `$ATTRIBUTE_LIST` `$EA`** — uzantı kaydı named stream (`AttributeListPullsEaFromExtensionRecord`).
111. **NTFS `$ATTRIBUTE_LIST` `$REPARSE_POINT`** — uzantı print name (`AttributeListPullsReparseFromExtensionRecord`).
112. **UDF EFE create** — Extended File Entry Create Date @+104 `createdAt` (`ExtendedFileEntryCreateTimeSetsCreatedAt`).

## R-Studio/UFS'e karşı hâlâ açık (önceliklendirilmiş yol haritası)

| # | Eksik | Rakip karşılığı | Önerilen yaklaşım | Efor |
|---|---|---|---|---|
| 1 | Btrfs / ZFS / F2FS / UFS ayrıştırıcıları | UFS Explorer tam destek | **v1 dışı** (belge). Btrfs önce; ZFS bileşik — uzman sürüm. | Büyük |
| 5 | Ses önizleme (WAV dışı codec / ffmpeg proxy) | Disk Drill | WAV/ID3/Ogg prefix `<audio>` var; **yeni bağımlılık yok** — ffmpeg eklenmez | Küçük |
| 7 | Ağ üzerinden kurtarma | R-Studio network | **v1 dışı** (ajan modeli) | Büyük |
| 10 | Mac native APFS (Disk Drill 6) | Disk Drill Mac | **v1 dışı**; yalnız image/.E01 | Büyük |
| 11 | WinPE / bootable | Disk Drill / R-Studio | **v1 dışı** | Büyük |
| 12 | Kamera RAW oturum Mac↔Win | Disk Drill | **v1 dışı** | Orta |
| 13 | BitLocker TPM-only bypass | (yok; bilinçli) | **v1 dışı** | — |

## Bilinçli sınırlar (rekabet öncesi tavanlar, belgeli)

- LZNT1: gerçek Windows sıkıştırmalı blob fixture yok (yalnız el yapımı akış)
- ZFS: bileşik pools+oremap — uzman sürüm kapsamı
- Kagıt holder: cache/dedup — sınırlı kaynakla OK

## v1 dışı (uygulama yok; belge)

- Btrfs / ZFS / F2FS / UFS ayrıştırıcı
- Mac native APFS, Recovery Vault, iOS
- Ağ ajanı, WinPE
- BitLocker TPM-only bypass
- Kamera RAW oturum Mac↔Win
- POSIX `openDrive`, H.264 decode, `$INDEX_BITMAP` skip-filter

## Sonuç

Lab’de NTFS MFT (2M in-suite peak <700MB) + yedek boot (son sektör) + `$MFTMirr` rec0 + `$ATTRIBUTE_LIST` uzantı `$DATA`/ADS/`$EA`/`$REPARSE_POINT` (resident/non-resident) + `$Bitmap` rec6 `$MFT` run + MFT view recN `$MFT` `$DATA` run, FAT yedek-FAT undelete + yedek boot (sektör 6) + volume label (`fat_vol_label`) + exFAT yedek boot (sektör 12) + volume label 0x83 (`exfat_vol_label`), carve, RAID6 + RAID5 FS-imza şerit tespiti, RAID10 ayna-çift + FS şerit (XOR sahte RAID5 yok), RAID üye sıra rotasyonu (`memberOrder`), RAID5 mdadm 4 düzen (left/right × asym/sym), mdadm super1 (1.2/1.1/1.0 + csum + roller + LINEAR/JBOD), Intel IMSM MPB (RST RAID1/5), SNIA DDF (son-512 `0xDE11DE11` + vd_config RAID0/1/5/6/10), GPT yedek başlık (birincil silinince son LBA), LVM2 PV + linear/striped/mirror/`type=raid` LV (`raid1`/`raid0`/`raid5_ls`/`raid6`/`raid10`), ISO 9660 PVD (`CD001` LBA 16) + volume id (`iso9660_vol_id`) + recording date `modifiedAt` (Rock Ridge `TF` ezer) + Joliet UCS-2 + Rock Ridge `NM` + SL + El Torito `BOOT.IMG` (default + section 0x90/0x91) + multi-extent 0x80, UDF VRS+AVDP dizin (`BEA01`/`HELLO.TXT` + EFE/short_ad/long_ad + yedek AVDP + VAT + metadata partition + mirror + bitmap orphan + LVD `udf_vol_id` + PVD `udf_pvd_id` + FSD `udf_fsd_id` + FE mtime + EFE create), APM `ER`/`PM` (512/2048 B), split RAW `.001`/`.002` concat, BitLocker (anahtarlı), LUKS1 parola (AES-XTS-plain64 128/256 + PBKDF2-SHA256; MBR/GPT/APM bölüm; LUKS2/Argon2 yok), Windows LDM public + PRT3 offset + spanned aynı-disk/çok-disk (`assembleLdm`) + DSK3/DSK4 GUID bind (replicated VBLK) + CMP3 stripe RAID0/RAID5, Storage Spaces `SPACEDB ` keşif (slab birleştirme yok), RAID1E near (tek N, FS imza), E01 zlib (HTTP `.E02` kardeş), imaj resume, XFS, VSS, USN, sabit/dinamik VHD, VHDX (log replay + relative parent), sparse/zlib VMDK, VDI type 4, QCOW2 (zlib küme + backing), DMG/UDIF UDRW+UDZO+ADC (`attachVhdFile`+clone), hex arama, MFT görüntüleyici, `$REPARSE_POINT` symlink print name, `$EA` named stream, EFS keşif, `$OBJECT_ID` GUID keşif, `$VOLUME_NAME` keşif, jbd2 isim + committed replay (`ext4_journal_replay`) + yedek super (4 MiB dışı) + yedek GDT + `s_volume_name` (`ext4_vol_name`), XFS AG1 yedek `XFSB` + `sb_fname` (`xfs_vol_name`) + `di_mtime` `modifiedAt`, HFS+ unused slot + journal katalog (`hfs_journal`) + yedek VH + resource fork (`name.rsrc`, overflow `0xFF`) + volume name (`hfs_vol_name`) + catalog dates, APFS yedek NXSB (son 4096) + inode create/mod, ReFS yedek SUPB (son küme + last-1), unalloc INDX JPEG/PNG/PDF/ZIP carve, WAV/ID3/Ogg ses önizleme, JPEG/PDF/ZIP onarım sidecar (`*_repaired`, orijinal MD5 durur), X3F SECd bound, TIFF Make/DNG etiket, HTTP HEAD-less Range, Unicode imaj hedefi ve kaynağı, inceleyici RAW/E01/VHD tarama (`driveIndex -2`), metadata tembel content-hash + “aynı içerik” rozeti, kanıt imajından RAW clone, RAID üye imaj birleştirme + imajdan geometri tespiti lab vektörleriyle doğrulanır. Bu Disk Drill / R-Studio paritesi değildir. Saha A/B/C PASS yalnız kullanıcı USB. Mac native yok. ffmpeg WAV proxy yeni bağımlılık olduğu için v1’de açık kalır. POSIX RAID okuma dalı kodda; bu makinede Linux runtime yok.
