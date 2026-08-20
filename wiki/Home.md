# Byteback Wiki

**Byteback** is a professional Windows forensic imaging and data recovery application:
native C++17 engine + Electron/React examiner UI.

| Page | Description |
|------|-------------|
| [Getting Started](Getting-Started.md) | Install, build, run, first scan |
| [Architecture](Architecture.md) | Engine layers, modules, data flow |
| [Security](Security.md) | Trust boundaries, threat model, limits |
| [Roadmap](Roadmap.md) | Now / Next / Later milestones |
| [Troubleshooting](Troubleshooting.md) | Common failures and fixes |
| [FAQ](FAQ.md) | Short answers |

## Links

- **Repository:** https://github.com/batu3384/byteback
- **Issues:** https://github.com/batu3384/byteback/issues
- **Security:** report via [Security](Security.md) (private repo — contact maintainer)
- **Contributing:** see `CONTRIBUTING.md` in the repo (git identity required)

## Status snapshot

| Area | State |
|------|--------|
| Native tests | 260+ `ctest` cases (3 optional skips) |
| TypeScript | `tsc` + Vitest green |
| CI workflow | Local `.github/workflows/build.yml` (push needs `workflow` OAuth scope) |
| Latest audit | `docs/codebase-audit/2026-08-20-adversarial.md` — **CONCERNS** (documented limits) |
