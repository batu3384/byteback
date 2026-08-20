# Security

## Threat model (summary)

| Boundary | Risk | Mitigation |
|----------|------|------------|
| Renderer ↔ Main (IPC) | Privilege abuse, forged recover targets | `contextIsolation`, allowlisted preload; recover DB-only IDs |
| Main ↔ Native | Memory corruption, arbitrary disk read | Native in main process only; bounded `readSectors` |
| HTTP image URL | SSRF to internal networks | `httpUrlHostAllowed` blocks private/link-local hosts |
| Imaging destination | Write to unintended path | Main-process allowlist (`allowed-image-dest.json`) |
| NSRL path | Path injection | Main-process file dialog only |
| PhysicalDrive wipe | Destructive write | Serial match + typed confirm + OS dialog; serialized with scan |

## Engine read/write policy

- **Read:** evidence disks via `GENERIC_READ`. May use `FILE_SHARE_WRITE` fallback when volume locked — not a write grant; use hardware write blockers for court copies.
- **Write:** only to user-chosen directories (recover, image) or explicit wipe flows. Engine does not format or patch source volumes during scan.

## Documented product limits (not vulnerabilities)

- BitLocker: FVEK hex, **0x0800 recovery password**, or **0x2000 user password** (Windows) — no TPM-only / startup-key bypass.
- APFS: partial container walk — not full snapshot parse.
- VSS: quick-scan shadow-copy enumeration — not full snapshot mount workflow.
- SSD TRIM: deep carve after TRIM is physically unreliable — UI warns.
- SSD / file wipe ≠ NIST 800-88 sanitization.

## Reporting

Private repository: open a maintainer-confidential channel or GitHub Security Advisory when enabled. Do not commit secrets, FVEK keys, or customer disk images.

Audit trail: SHA-256 chained log at `<userData>/byteback.db.audit.log`.

See also `docs/codebase-audit/` adversarial reports in the repo.
