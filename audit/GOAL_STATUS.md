# Thread goal status (audit roadmap)

**Not complete** — Phase 4 requires operator hardware and signing secrets this repo cannot supply.

## Engineering (objective items 1–7 + honest gaps doc)

All **Done** with tests or CI. Evidence: `audit/OBJECTIVE_CHECKLIST.md`. Gate: `npm run verify:audit`.

Last verified: 2026-10-28 20:20 local (`verify:audit:full` exit 0 — 490 Vitest, native ctest subset 5/5, posix verify; `phase4:status` exit 1 expected).

## Phase 4 (objective item 8 — product / release)

| Item | Blocker | Next action |
|------|---------|-------------|
| Field USB A/B/C | Physical media | `scripts/field-usb-preflight.ps1` → fill `audit/FIELD_USB_LOG.md` |
| Authenticode | `CSC_*` on release machine | `scripts/release-dist.ps1` or GitHub `release-signed.yml` + secrets → `signtool verify` |
| NSIS clean-box | Fresh VM install test | Lab unsigned `release/Byteback Setup 0.1.0.exe` (built 2026-10-28) — VM smoke still `[ ]` |
| Tagged v1.0 | Above gates | Bump version, tag after checklist PASS |
| Streaming imaj (§8) | Product roadmap | Documented in `HONEST_PRODUCT_GAPS.md`; not part of audit code fixes |

## Agent blocked (Phase 4)

Engineering items in the thread objective are **Done** (`OBJECTIVE_CHECKLIST.md`, `npm run verify:audit`).

Further goal progress requires **you** — step-by-step: `audit/PHASE4_OPERATOR_RUNBOOK.md`. Progress probe: `npm run phase4:status`.

Until those are honestly PASS, do **not** set the thread goal to complete.

## Agent idle (Phase 4)

No further repo-only work closes the thread goal. **`npm run phase4:status` must exit 0** (all six release-checklist Phase 4 rows `[x]` with real USB log / signtool / VM evidence) before `UpdateGoal` complete.

## Pause vs complete

- **Do not** mark the thread goal complete until checklist rows A/B/C and code sign are honestly PASS.
- To **pause** the goal after engineering: pause/stop the goal in Cursor (agent cannot call UpdateGoal for pause).
- **Closure probe:** `npm run verify:goal` (engineering + `phase4:status`; exit 0 only when checklist Phase 4 rows are `[x]`).
