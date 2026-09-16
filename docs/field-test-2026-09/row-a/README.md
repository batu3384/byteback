# Satır A — NTFS HDD

**Sıra:** B ve C’den sonra. Tavan: deep/full-carve ≤500GB veya ≤500K dosya.

**Durum:** bekliyor (kullanıcı medyası). Lab unalloc INDX golden saha A PASS değil.

## Senaryo

İkincil NTFS HDD. Quick + deep + (tavan içinde) full-carve. Silinmiş klasör adı Extra Found / `$I30` unalloc ile karşılaştırılabilir not.

## Beklenen

Tarama tamamlanır, faz % akar, yanlış-pozitif görsel incelemede düşük. Ham `INDX` magic satır üretmez.

## Kayıt

`RESULT.md` + screenshot + session.log + audit.
