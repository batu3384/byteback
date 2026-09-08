# Saha Test Kayıtları — 2026-09 (Faz 0.4)

Durum: **hazırlık tamam, fiziksel satırlar kullanıcı donanım oturumu bekliyor.**
Protokol: `docs/field-test-protocol.md`.

| ID | Sorumlu | Durum | Not |
|----|---------|-------|-----|
| A | kullanıcı + icraacı | bekliyor | NTFS HDD — fiziksel |
| B | kullanıcı + icraacı | bekliyor | FAT32 USB — fiziksel |
| C | kullanıcı + icraacı | bekliyor | exFAT SD — fiziksel |
| D | icraacı | bekliyor | bozuk-MFT imajı — uygulamayla koşturulacak |
| E | kullanıcı + icraacı | bekliyor | kötü bölgeli USB — opsiyonel |
| F | icraacı | bekliyor | E01 roundtrip — uygulamayla koşturulacak |
| G | icraacı | bekliyor | yazılım RAID5 imajları — uygulamayla koşturulacak |
| H | kullanıcı + icraacı | bekliyor | BitLocker — kurtarma anahtarı gerekli |

Her satır koşulduğunda bu klasör altına `row-<id>/` (screenshot + log + doldurulmuş matris satırı) eklenir ve buradaki durum güncellenir.
