# Adversarial review — Byteback rebrand + repo düzeni

- Tarih: 2026-08-20 (güncelleme: düz repo yapısı)
- Kapsam: Tüm repo — kaynak repo kökünde, `docs/`, `.github/`
- Yöntem: Saboteur / New Hire / Security Auditor
- Runtime: `tsc` OK, Vitest 36/36
- Verdict: **CONCERNS** (bilinçli spek sınırları duruyor)

## Önceki BLOCK maddeleri — durum

| ID | Konu | Durum |
|----|------|--------|
| AR20-001 | StopScan TSFN deadlock | **Kapalı** |
| AR20-002 | Recover renderer `runs` bypass | **Kapalı** |
| AR20-003 | Resident NTFS kurtarma | **Kapalı** |
| Rebrand | wolf-recovery kalıntısı | **Kapalı** |
| Repo yapısı | `disk/byteback/` iç içe klasör | **Kapalı** — uygulama repo kökünde |

## Kalan uyarılar

- Paylaşımlı `diskReader_` modeli
- VSS / APFS derinliği / BitLocker decrypt sınırları (README'de belgelenmiş)

## Summary

Ürün kodu artık repo kökünde (`native/`, `src/`, `package.json`). GitHub repo adı `byteback` olmalı; yerel klasör adı (`disk` vb.) clone tercihine bağlı.
