# NTFS Akışkan Emit (Faz 1.1) — Görev Planı

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Adımlar `- [ ]` izlemeli.
> **Spec:** `docs/superpowers/plans/2026-09-09-master-roadmap.md` §1.1 — iki tasarım kuralı zorunlu: (K1) orphan emit korunur, (K2) emit zamanlaması erken/edeğişmez.

**Hedef:** `ntfs_parser.cpp`'nin Pass-1'deki tüm `tempFiles` (FileRecord + residentData) RAM birikimini kaldır; kompakt `MftIndex` + Pass-2 akışkan emit. 2M dosyada native peak RSS < 700MB ve bellek artışı kayıt sayısıyla lineer DEĞİL.

**Ön koşullar:** Sahiplik: `native/src/fs/ntfs_parser.cpp`, `native/include/byteback_fs.h` (yalnız NTFSParser bölümü), `native/tests/test_ntfs_parser.cpp`, `native/tests/test_e2e_scan_recover.cpp`. Commit yok; kapılar merkezde.

## Görev A: Ölçüm temeli (kırmızı kanıt)

- [ ] A1. `test_ntfs_parser.cpp`'e peak-RSS yardımcısı ekle (Windows): `GetProcessMemoryInfo` — tarama öncesi/sonrası `PeakWorkingSetSize`.
- [ ] A2. Mevcut kodda 200K sentetik NTFS fixture üret (mevcut fixture üretici deseniyle; kayıt başına ~1KB resident-data içersin ki bugünkü birikim gerçeği yansıtsın) → peak RSS'i yazdır. Beklenen: yüzlerce MB (kırmızı kanıt). Bu ölçümü rapora yaz.
- [ ] A3. Commit: `test(ntfs): memory baseline for streaming emit (red evidence)`.

## Görev B: MftIndex (kompakt kayıt dizini)

- [ ] B1. `byteback_fs.h` NTFSParser private bölümüne: `struct MftEntry { uint64_t byteOffset; uint32_t byteLen; uint64_t mftRef; uint8_t flags; std::vector<uint8_t> packedRuns; }` + `std::unordered_map<uint64_t, MftEntry>` (packedRuns = `runs_codec` serialize; resident data İÇERMEZ).
- [ ] B2. Pass-1'i refactor et: FileRecord kurma + `tempFiles.push_back` KALDIRILIR; yalnız MftEntry doldurulur. Resident-data blob'ları diskte kalır (Pass-2 okur).
- [ ] B3. Derle + mevcut süitte yalnız derleme kırılmalarını onar (davranış henüz bozuk olabilir — Pass-2 gelmeden emit yok; testler B6'ya kadar kırmızıdır, bu kabul).
- [ ] B4. Commit: `refactor(ntfs): pass-1 builds compact MftIndex only (WIP)`.

## Görev C: Pass-2 akışkan emit (K1 dahil)

- [ ] C1. Pass-2 dizin yürüyüşü: her dizin girdisi için `MftIndex`'ten montaj → `callback` (mevcut emitFile semantiği birebir: path, confidence, status, runs, startByteOffset).
- [ ] C2. `visited_` seti (unordered_set<uint64_t>): Pass-2'de emit edilen ino'lar işaretlenir.
- [ ] C3. **K1 orphan sweep:** Pass-2 bittikten sonra `MftIndex`'te visited-olmayan her giriş orphan olarak emit edilir (isim boş/kaynak "orphan", conf bandı mevcut orphan semantiğiyle aynı). Sıra: deterministik (ino sıralı).
- [ ] C4. Resident data: Pass-2 montajında ilgili MFT kaydı tekil okunup `$DATA` resident akıştan çıkarılır (mevcut parse fonksiyonu yeniden kullanılır).
- [ ] C5. İptal: her dizin bloğu sonunda `isRunning` (mevcut desen).
- [ ] C6. Commit: `feat(ntfs): pass-2 streaming emit with orphan sweep`.

## Görev D: Eşdeğerlik + bellek + resume kanıtları

- [ ] D1. **Kayıp yok (K1):** sentetik hacimde `emit sayısı == MftIndex girişi` (named+orphan toplamı); eski kodun çıktısıyla multiset eşitliği (isim/boyut/runs/conf bandı).
- [ ] D2. **Bellek:** 200K ve 2M noktalarında peak RSS ölç; lineer büyüme REDDEDİLİR (200K→2M 10× kayıtta peak artışı < 2×). Hedef: 2M'de < 700MB.
- [ ] D3. **Resume (39aa491 semantiği):** yarıda kesilen tarama → resume → `DedupIndex` rehydrate eder; ikinci turda çifte kayıt YOK (resume testi mevcut; yeşil kalır).
- [ ] D4. Commit: `test(ntfs): streaming equivalence + memory growth + resume`.

## Görev E: Kapı

- [ ] E1. Tam native süit yeşil (523+); posix-syntax job kapsamı korunur (`ntfs_parser.cpp` zaten 54 TU listesinde — çapraz-platform derleme bozulmaz).
- [ ] E2. Rapor: A2 kırmızı kanıt ölçümü → D2 sonrası ölçüm; K1/K2 kanıtları; tavanlar.
