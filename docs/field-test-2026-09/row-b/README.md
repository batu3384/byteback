# Satır B — FAT32 USB 5 silinmiş foto (P1 kapısı)

**Sıra:** B, sonra C, sonra A. B 5/5 MD5 geçmezse Program 1 parite iddiası yok; motor triyaj.

**Durum:** bekliyor (kullanıcı medyası). Lab golden `Fat32FiveDeletedJpegMd5` saha PASS yerine geçmez.

## Donanım

FAT32 USB 8–32 GB. Yazma-engelleyici varsa tak. Kaynak salt-okunur.

## Hazırlık (kullanıcı)

1. USB’yi yedekle. Test verisi silinecek.
2. 5 JPEG kopyala; her birinin MD5’ini kaydet (`certutil -hashfile foto.jpg MD5`).
3. 5 dosyayı sil (Geri Dönüşüm atlat: Shift+Delete). Çıkar, tak.

## Uygulama

1. Byteback başlat. Vaka Görünümü → kanıt yollarını `docs/field-test-2026-09/row-b/` altına not et.
2. USB seç, **quick** tarama, sonra gerekirse **deep**.
3. 5 fotoğu kurtar. Hedef ASCII olmayan klasör de dene (`C:\Çıkış\`).
4. Kurtarılan 5 dosya vs kaynak MD5.

## Kayıt

`RESULT.md` doldur. PASS yalnız 5/5 byte-exact. Ekran + `session.log` + audit.log.

## Fail triyaj

- 0/5: FAT walk / 0xE5 / LFN. Lab golden kırmızı mı bak.
- k/5 k<5: hangi isim, carve vs fat kaynağı.
- MD5 kayması: ilk küme / kısmi kurtarma / TRIM.
