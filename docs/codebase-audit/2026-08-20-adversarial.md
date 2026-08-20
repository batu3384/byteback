# Adversarial review — Byteback rebrand + repo layout

- Date: 2026-08-20 (update: flat repo layout)
- Scope: Full repo — source at repo root, `docs/`, `.github/`
- Method: Saboteur / New Hire / Security Auditor
- Runtime: `tsc` OK, Vitest 36/36
- Verdict: **CONCERNS** (documented spec limits remain)

## Previous BLOCK items — status

| ID | Topic | Status |
|----|------|--------|
| AR20-001 | StopScan TSFN deadlock | **Closed** |
| AR20-002 | Recover renderer `runs` bypass | **Closed** |
| AR20-003 | Resident NTFS recovery | **Closed** |
| Rebrand | wolf-recovery residue | **Closed** |
| Repo layout | nested `disk/byteback/` folder | **Closed** — application at repo root |

## Remaining warnings

- Shared `diskReader_` model
- VSS / APFS depth / BitLocker decrypt limits (documented in README)

## Summary

Product code now lives at repo root (`native/`, `src/`, `package.json`). GitHub repo name should be `byteback`; local folder name (`disk` etc.) depends on clone preference.
