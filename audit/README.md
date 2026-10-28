# Audit remediation (2026-10-28)

| Doc | Purpose |
|-----|---------|
| [AUDIT_REPORT.md](AUDIT_REPORT.md) | Original findings |
| [NOTES.md](NOTES.md) | Working notes snapshot |
| [REMEDIATION.md](REMEDIATION.md) | Phase 1–3 done evidence; Phase 4 open |
| [OBJECTIVE_CHECKLIST.md](OBJECTIVE_CHECKLIST.md) | Thread goal ↔ status |
| [GOAL_STATUS.md](GOAL_STATUS.md) | Agent blocked on Phase 4 |
| [HONEST_PRODUCT_GAPS.md](HONEST_PRODUCT_GAPS.md) | Non-parity claims |
| [PHASE4_OPERATOR_RUNBOOK.md](PHASE4_OPERATOR_RUNBOOK.md) | USB / sign / NSIS / tag |
| [FIELD_USB_LOG.md](FIELD_USB_LOG.md) | Field USB A/B/C evidence template |
| [PHASE4_SIGN_LOG.md](PHASE4_SIGN_LOG.md) | Authenticode / signtool evidence template |
| [PHASE4_VM_SMOKE_LOG.md](PHASE4_VM_SMOKE_LOG.md) | Clean-box NSIS evidence template |

**Engineering gate:** `npm run verify:audit` (CI Linux). **Windows:** `npm run verify:audit:full`. **Phase 4 blockers:** `npm run phase4:status`. **Thread goal probe:** `npm run verify:goal`.
