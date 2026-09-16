# Saha Test Kayıtları — 2026-09 (Faz 0.4 + Program 1)

Durum: **hazırlık tamam, fiziksel satırlar kullanıcı donanım oturumu bekliyor.** D/F/G için **lab gtest kanıtı** var; bu, protokoldeki uygulama-UI saha oturumunun yerine geçmez.
Program 1: **B sonra C sonra A**. B 5/5 MD5 yoksa parite iddiası yok. Mac / Disk Drill Mac yok.
Protokol: `docs/field-test-protocol.md`. Kit: `row-b/`, `row-c/`, `row-a/` (`RESULT.md` DURUM: bekliyor).

| ID | Sorumlu | Durum | Not |
|----|---------|-------|-----|
| A | kullanıcı + icraacı | bekliyor | NTFS HDD — fiziksel |
| B | kullanıcı + icraacı | bekliyor | FAT32 USB — fiziksel |
| C | kullanıcı + icraacı | bekliyor | exFAT SD — fiziksel |
| D | icraacı | lab-kanıt | gtest `NtfsParser.OrphanSweepEmitsEntriesLostToMidScanReadFailure` — bozuk/okunamayan MFT kaydı orphan+low-conf; UI saha oturumu yok |
| E | kullanıcı + icraacı | bekliyor | kötü bölgeli USB — opsiyonel |
| F | icraacı | lab-kanıt | gtest `DiskImagerTest.EwfImageCarriesDigestAndRereadsIdentical` + `DiskImagerResume.CancelThenResumeProducesByteExactImage`; UI imager oturumu yok |
| G | icraacı | lab-kanıt | gtest `RaidDetect.DetectsRaid5BlockSizeAndConfidence`; Virtual RAID UI oturumu yok |
| H | kullanıcı + icraacı | bekliyor | BitLocker — kurtarma anahtarı gerekli |

Her satır koşulduğunda bu klasör altına `row-<id>/` (screenshot + log + doldurulmuş matris satırı) eklenir ve buradaki durum güncellenir. Lab-kanıt satırları `geçti` sayılmaz; Faz 0.4 kapısı A/B/F UI saha kaydı ister.
