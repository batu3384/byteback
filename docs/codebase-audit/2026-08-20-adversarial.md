# Adversarial review — Byteback rebrand + repo bütünlüğü

- Tarih: 2026-08-20 (güncelleme: rebrand tamamlama)
- Kapsam: Tüm repo — `byteback/` kaynak, `docs/`, `.github/`, git durumu
- Yöntem: Saboteur / New Hire / Security Auditor
- Runtime: `tsc` OK, Vitest 36/36, ctest 266/266 geçti (3 skip)
- Verdict: **CONCERNS** (önceki BLOCK maddelerinin çoğu kapanmış; bilinçli spek sınırları ve operasyonel riskler duruyor)

## Önceki BLOCK maddeleri — durum

| ID | Konu | Durum |
|----|------|--------|
| AR20-001 | StopScan TSFN deadlock | **Kapalı** — `StopScan` yalnız `requestStop()`; callback `NonBlockingCall` |
| AR20-002 | Recover renderer `runs` bypass | **Kapalı** — `RecoverFile(drive, fileId, destDir, scanId)` DB-only |
| AR20-003 | Resident NTFS kurtarma | **Kapalı** — `ntfs_parser` `residentData` dolduruyor; `recovery_engine` yazıyor |
| Rebrand | wolf-recovery kalıntısı | **Kapalı** — kaynak/CI/package `byteback`; git rename commit edildi |

## Kalan uyarılar (merge edilebilir, bilinçli risk)

### AR20-004 | Paylaşımlı `diskReader_`

- Tek reader; hex / RAID / recover sırası çakışabilir. `tryBeginHeavyOp` ağır işleri serileştirir; ince okuma yolları hâlâ dikkat ister.

### AR20-008 | VSS / APFS / BitLocker decrypt

- README ve ARCHITECTURE dürüst sınırları listeler. VSS quick scan sentinel; tam mount yok.

### AR20-011 | Dist smoke CI'da koşuluyor

- `build.yml` Playwright + `electron-builder --win dir` adımı mevcut.

### IPC hata maskeleme

- **Kapalı** — `ipc-native.ts` `callNative`; kritik handler'lar native hatada reject eder.

## Notlar

- `FileRecord.id` parser sayacı ≠ SQLite rowid; recover/preview DB id kullanır (doğru yol).
- Belgeler: repo kökü `docs/` (denetim + planlar), `byteback/docs/` (ARCHITECTURE).
- Kök `README.md` clone giriş noktası.

## Summary

Byteback rebrand kaynak ve CI düzeyinde tamam. Önceki üretim BLOCK'ları (durdur deadlock, recover bypass, resident extract) kapanmış. Kalan CONCERNS bilinçli forensic sınırları (VSS, APFS derinliği, BitLocker) ve paylaşımlı reader modeli — README'de belgelenmiş.
