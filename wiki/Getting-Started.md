# Getting Started

## Requirements

- Windows 10/11 x64
- **Administrator** elevation (PhysicalDrive access)
- Visual Studio 2022 Build Tools — “Desktop development with C++”
- Node.js 20+
- CMake 3.20+
- [GitHub CLI](https://cli.github.com/) (optional, for wiki sync)

## Clone and identity

```powershell
git clone https://github.com/batu3384/byteback.git
cd byteback
powershell -ExecutionPolicy Bypass -File scripts/setup-git-identity.ps1
npm install
```

Run `setup-git-identity.ps1` after every clone so commits attribute to your GitHub account (not a bot identity).

## Development session

```powershell
npm run dev
```

Builds the native addon (`cmake-js`) and starts Electron with hot reload.

## Production build

```powershell
npm run build
npm run dist          # NSIS x64 installer → release/
```

## Verify

```powershell
npm run typecheck
npm run test
npm run test:native   # requires: cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
npm run test:e2e      # requires npm run build first
```

## First examiner workflow

1. Launch as Administrator.
2. **Dashboard** — pick a drive, choose scan profile (Quick / Deep / Full carve).
3. On SSD + Deep/Full carve, confirm TRIM warning if shown.
4. **Results** — recover via SQLite `fileId` + `scanId` (not raw sector runs from UI).
5. **Imager** — pick allowlisted destination path, RAW or E01, verify MD5 panel.

See [Architecture](Architecture) for engine boundaries and [Security](Security) for write paths.
