# Byteback

**Windows için profesyonel adli bilişim ve veri kurtarma uygulaması.** Ham disk
erişimini C++17 native motorla, Electron/React arayüzüyle birleştirir.

## Hızlı başlangıç

```bash
cd byteback
npm install
npm run dev
```

Yönetici olarak çalıştırın (PhysicalDrive erişimi için). Detaylı özellik listesi,
komutlar ve güvenlik notları: **[byteback/README.md](byteback/README.md)**.

## Depo yapısı

```
disk/
├── .github/workflows/     # CI: typecheck, Windows native build, ctest, Playwright
├── docs/                  # Denetim raporları, yol haritası planları, spesifikasyonlar
└── byteback/              # Uygulama kaynağı
    ├── docs/              # Mimari belgesi (ARCHITECTURE.md)
    ├── native/            # C++ motor + GoogleTest
    ├── src/               # Electron main, preload, React renderer
    └── package.json
```

## Belgeler

| Konu | Dosya |
|------|--------|
| Kullanıcı kılavuzu ve komutlar | [byteback/README.md](byteback/README.md) |
| Motor mimarisi | [byteback/docs/ARCHITECTURE.md](byteback/docs/ARCHITECTURE.md) |
| Kod tabanı denetimleri | [docs/codebase-audit/](docs/codebase-audit/) |
| Geliştirme yol haritası | [docs/superpowers/](docs/superpowers/) |

## CI

GitHub Actions iş akışı repo kökündeki `.github/workflows/build.yml` dosyasında
tanımlıdır; `working-directory: byteback` ile uygulama dizininde çalışır.

## Lisans

MIT License — ayrıntılar [byteback/README.md](byteback/README.md).
