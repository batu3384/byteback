# FAQ

## What is Byteback?

Windows forensic imaging + data recovery: read-only engine, Electron examiner UI, hash-chained audit log.

## Does it write to evidence disks?

No intentional writes to source media. Recovery and images go to paths you choose. Wipe is a separate, confirmed destructive flow.

## BitLocker?

Detect and scan with **FVEK hex** or **clear-key recovery password** only. No TPM/password cracking.

## macOS / Linux?

Windows-first. Native I/O is `\\.\PhysicalDriveN`. POSIX stub exists for memory/EWF tests only.

## How many file signatures?

~114 built-in; optional `resources/signatures-extended.json` merge (~247 in recovery-mastery tests).

## Can I recover from a RAID array?

Virtual RAID 0/1/5/6/10 in UI. Mark failed members with `fail_disk`. Not hardware RAID controller firmware.

## Is APFS fully supported?

Discovery + partial extent recovery. Full APFS container snapshot walk is **not** shipped — see [Roadmap](Roadmap).

## Where are scans stored?

`<userData>/byteback.db` (Electron app data). Audit log: `byteback.db.audit.log`.

## Open source?

MIT License. Repository: https://github.com/batu3384/byteback (private as of 2026-08).

## How do I update the wiki?

Edit files in `wiki/` in the main repo, then run `scripts/sync-wiki.sh`. See `wiki/README.md`.
