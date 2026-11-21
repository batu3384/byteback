import { describe, expect, it } from 'vitest'
import {
  auditReportChainKind,
  auditReportLinesKind,
  shouldEmitAuditReportSection,
  type AuditReportProbe,
  reportStamp,
} from './report-audit'

const okChain = { ok: true, entries: 12, brokenAt: -1, detail: '' }
const brokenChain = { ok: false, entries: 4, brokenAt: 2, detail: 'hash mismatch' }

function probe(partial: Partial<AuditReportProbe>): AuditReportProbe {
  return {
    linesUnread: false,
    lines: [],
    chainUnread: false,
    chain: null,
    ...partial,
  }
}

describe('audit report honesty', () => {
  it('does not treat an unread log as an empty event list', () => {
    const p = probe({ linesUnread: true })
    expect(auditReportLinesKind(p)).toBe('unread')
    expect(shouldEmitAuditReportSection(p)).toBe(true)
  })

  it('omits the section only when both probes succeeded empty', () => {
    const p = probe({})
    expect(auditReportLinesKind(p)).toBe('empty')
    expect(auditReportChainKind(p)).toBe('absent')
    expect(shouldEmitAuditReportSection(p)).toBe(false)
  })

  it('keeps a chain unread verdict even when there are no lines', () => {
    const p = probe({ chainUnread: true })
    expect(auditReportChainKind(p)).toBe('unread')
    expect(shouldEmitAuditReportSection(p)).toBe(true)
  })

  it('emits present lines plus a live chain verdict', () => {
    const p = probe({ lines: ['SCAN | id=1'], chain: okChain })
    expect(auditReportLinesKind(p)).toBe('present')
    expect(auditReportChainKind(p)).toBe('ok')
    expect(shouldEmitAuditReportSection(p)).toBe(true)
  })

  it('emits a broken chain without dropping the event table', () => {
    const p = probe({ lines: ['SCAN | id=1'], chain: brokenChain })
    expect(auditReportChainKind(p)).toBe('broken')
    expect(shouldEmitAuditReportSection(p)).toBe(true)
  })
})

describe('reportStamp', () => {
  it('uses the local calendar day and clock, not the UTC day', () => {
    const d = new Date(2026, 10, 21, 1, 30, 5) // 01:30 local — still the previous day in UTC east of Greenwich
    expect(reportStamp(d)).toEqual({ date: '2026-11-21', time: '01:30:05' })
  })
})
