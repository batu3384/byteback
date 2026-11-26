# Thread goal status (audit roadmap)

## Engineering — complete

- Audit items 1–7: **Done** with tests. Evidence: `audit/OBJECTIVE_CHECKLIST.md`.
- Remediation round 2 (K1–K7, Y1–Y9): **Done** — APFS real B-tree, ext4
  journal/slack, FAT paths/overwrite guards, NTFS LZNT1 units, ddrescue-style
  skip-ahead + trim imaging, honest TRIM/ReFS/carving behavior. Commits
  `eed493f..f881362`; adversarial rounds 1–2 closed `3a304bc`, `876d90e`.
- Parity round: Btrfs parser, ~230 carve signatures, RFC 3394 AES-KW
  (FileVault prerequisite) — commits `f7acd9d`, `a1f56f4`, `e73eb7b`.
- Gates: native ctest 970 passing (4 optional hardware/bench skips),
  Vitest 500/500, typecheck clean, Playwright e2e 45/45.

## Phase 4 (objective item 8 — product / release) — operator-gated, unchanged

| Item | Blocker | Next action |
|------|---------|-------------|
| Field USB A/B/C | Physical media | `scripts/field-usb-preflight.ps1` → fill `audit/FIELD_USB_LOG.md` |
| Authenticode | `CSC_*` on release machine | `scripts/release-dist.ps1` or GitHub `release-signed.yml` + secrets → `signtool verify` |
| NSIS clean-box | Fresh VM install test | Lab unsigned installer — VM smoke still `[ ]` |
| Tagged v1.0 | Above gates | Bump version, tag after checklist PASS |

Closure probe: `npm run verify:goal`. Do **not** claim release readiness until
the checklist rows are honestly PASS.
