# Troubleshooting

## Native addon fails to load

**Symptoms:** `Native engine error` in UI; `byteback_engine.node` missing.

```powershell
npm run build:native
# expect: native/build/Release/byteback_engine.node
```

Install VS 2022 Build Tools with C++ workload. CMake 3.20+ on PATH.

## No drives listed

**Symptoms:** Empty dashboard, “no physical drives” message.

- Run Byteback **as Administrator**.
- Check IPC error in DevTools console — native failure now throws instead of returning `[]`.
- USB bridges and locked volumes may hide members.

## Deep scan on SSD / TRIM warning

**Symptoms:** Modal blocks Deep or Full carve.

Expected: SSD TRIM makes deleted clusters unrecoverable. Confirm modal sets `allowSsdDeepScan: true` only when examiner accepts risk.

## Recover fails “no data runs”

**Symptoms:** Small NTFS files fail recover.

- Ensure recover uses **Results** flow with valid `scanId` + `fileId`.
- Resident `$DATA` requires parser `residentData` path (fixed AR20-003). Update to latest `main`.

## `test:native` — project file missing

```powershell
cmake -S native -B native/build -DBYTEBACK_BUILD_TESTS=ON
npm run test:native
```

## E2E / Playwright skip

Build first: `npm run build` → `out/main/main.js` must exist.

## Git commits show bot name

Run once per clone:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup-git-identity.ps1
```

## Wiki sync fails

- `gh auth login` with `repo` scope
- First publish: create any page on GitHub Wiki UI once, then run `scripts/sync-wiki.sh`
- Windows: use Git Bash or WSL (script requires `bash`, `rsync`, `git`)

## CI workflow push rejected

```
refusing to allow an OAuth App to create or update workflow ... without workflow scope
```

```powershell
gh auth refresh -h github.com -s workflow
git push
```
