# Satır C — exFAT SD preservePaths

**Sıra:** B geçtikten sonra. B fail ise C yine koşulur ama parite iddiası yok.

**Durum:** bekliyor (kullanıcı medyası). Lab `ExFatDeletedPreservePathsMd5` saha PASS değil.

## Donanım

exFAT SD kart.

## Hazırlık

1. Klasör `pics\` içinde bir JPEG. MD5 kaydet.
2. Dosyayı sil (Shift+Delete). Kartı çıkar-tak.

## Uygulama

1. Quick tarama. `shot`/silinmiş JPEG.
2. Kurtarırken **Klasör yapısını koru**.
3. Hedefte `pics\<ad>` oluşmalı; MD5 kaynakla aynı.

## Kayıt

`RESULT.md`. Fail: path `/` kaldıysa exFAT DirJob; MD5 yoksa silinmiş zincir.
