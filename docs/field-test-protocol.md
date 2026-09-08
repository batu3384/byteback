# Byteback — Saha Test Protokolü (Master Roadmap Faz 0)

**Amaç:** Sentetik fixture'ların kapsayamayacağı gerçek donanım davranışını (firmware tuhaflıkları, USB köprüleri, sektör boyutları, gerçek dosya sistemleri) sistematik olarak doğrulamak ve kayıt altına almak.

**Rol ayrımı:** İcraacı (ajan) imaj-dosyası tabanlı satırları (D, F, G) kendi koşturur; fiziksel medya adımları (A, B, C, E, H) **kullanıcı eliyle** yapılır — icraacı uygulama tarafını yönlendirir ve sonuç kaydını toplar.

**Ön koşul — bilinen tavanlar (0.1 tablosu):** Faz 1.1 kapanana dek deep/full-carve yalnız ≤500GB veya ≤500K dosya beklentili hacimlerde koşturulur. 1M+ dosyalı disklerde deep scan atlanır, not düşülür: "beklenen sınır: RAM birikimi (Faz 1.1)". Faz 1 kapısı kapanınca bu tablo revize edilir ve tur tekrar koşulur.

**Genel kurallar:**
- Her satır öncesi uygulama yeniden başlatılır; userData temiz MI? — HAYIR, gerçek kullanım senaryosu: mevcut userData korunur (kalıcılık da test edilir). Yalnız satır başına yeni tarama başlatılır.
- Kayıt: ekran görüntüleri + `session.log` + `<db>.audit.log` + DB kopyası → `docs/field-test-2026-09/row-<id>/`.
- Kanıt yol göstergesi: Vaka Görünümü → "Kanıt dosya yolları" bölümünden kopyalanır (0.2 ile eklendi).
- Audit zinciri her satır sonunda Rapor sayfasından doğrulanır (report-chain-status yeşil).
- Yazma-engelleyici: donanım varsa takılı; yoksa `dash.evidenceNote` uyarısı göz önünde bulundurularak yalnız kesin salt-okunur kaynaklara dokunulur.

## Test matrisi

| ID | Donanım/ortam | Senaryo | Beklenen | Gerçek | Kanıt | Durum |
|----|--------------|---------|----------|--------|-------|-------|
| A | NTFS HDD (kullanıcı) | quick + deep + full-carve; ≤500GB | tarama tamamlanır, faz % akar, sonuç sayısı makul, yanlış-pozitif görsel incelemede düşük | | | bekliyor |
| B | FAT32 USB 8-32GB | 5 foto sil → kurtar → kaynak MD5 ile karşılaştır | kurtarılan 5/5, MD5 birebir, byte-exact | | | bekliyor |
| C | exFAT SD kart | quick scan + kurtarma | dizin yapısı korundu (preservePaths) | | | bekliyor |
| D | **imaj dosyası (icraacı)** | kasıtlı bozuk MFT'li imaj: sentetik NTFS imajında MFT başlık baytları bozulur → tarama | çökme yok; bozuk kayıtlar atlanır/low-conf; orphan path'ten kurtarılanlar listelenir | | | bekliyor |
| E | kötü bölgeli USB (kullanıcı; HDDScan ile işaretlenmişse) | deep scan | askı yok; badSectors UI'da görünür; iptal <10sn | | | bekliyor |
| F | **imaj dosyası (icraacı)** | E01 imaj al (kaynak: sentetik imaj dosyasından) → yeniden aç → verifyDigest + byte karşılaştırma | digest zorunlu geçer; byte-exact; resume iptal→devam çalışır | | | bekliyor |
| G | **imaj dosyaları x3 (icraacı)** | yazılım RAID5 imajları (sentetik üretim) → Otomatik Tespit → düzey+şerit doğru → tarama | RAID5 tespit edilir (FS-imza skoruyla şerit), tarama birebir | | | bekliyor |
| H | BitLocker'lı ikincil bölüm (kullanıcı; kurtarma anahtarı hazır) | anahtar kilidi açma → tarama | içerik listelenir; anahtar loglara sızmaz | | | bekliyor |

## Satır prosedürü şablonu

1. Donanımı/bağla (kullanıcı) / imajı hazırla (icraacı).
2. Uygulamayı başlat (`npm start`); Vaka Görünümü'nden kanıt yollarını kaydet.
3. Senaryoyu koştur; faz progress, ETA, canlı liste davranışını gözle.
4. İptal senaryosu: %40 civarı Durdur → <10sn'de durur mu? Resume çalışır mı?
5. Rapor sayfası: audit zinciri doğrulaması + PDF üretimi.
6. Kayıt klasörüne: screenshot'lar, session.log, audit.log, doldurulmuş matris satırı.

## Kabul

- A, B, F satırları geçmeden Faz 1 kapısı kapanamaz ("kritik geçmezse ilgili faz maddesi öne alınır" kuralı).
- Her "geçmedi" için: severity + ilgili faza bağlantı + yeniden triyaj girdisi.
