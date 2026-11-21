/** Probe of the hash-chained audit log for the generated HTML report. */

export type AuditChainStatus = {
  ok: boolean
  entries: number
  brokenAt: number
  detail: string
}

export type AuditReportProbe = {
  linesUnread: boolean
  lines: string[]
  chainUnread: boolean
  chain: AuditChainStatus | null
}

export type AuditReportLinesKind = 'unread' | 'empty' | 'present'
export type AuditReportChainKind = 'unread' | 'ok' | 'broken' | 'absent'

export function auditReportLinesKind(p: AuditReportProbe): AuditReportLinesKind {
  if (p.linesUnread) return 'unread'
  if (p.lines.length === 0) return 'empty'
  return 'present'
}

export function auditReportChainKind(p: AuditReportProbe): AuditReportChainKind {
  if (p.chainUnread) return 'unread'
  if (!p.chain) return 'absent'
  return p.chain.ok ? 'ok' : 'broken'
}

/** Unread is never “no events”. Empty success with no chain probe may omit. */
export function shouldEmitAuditReportSection(p: AuditReportProbe): boolean {
  return p.linesUnread || p.chainUnread || p.lines.length > 0 || p.chain != null
}

/** Report header stamp in the examiner's local time. The old header mixed the
 *  UTC date with the local clock, so a report made before 03:00 in Türkiye
 *  carried the previous day. */
export function reportStamp(d: Date): { date: string; time: string } {
  const p = (n: number) => String(n).padStart(2, '0')
  return {
    date: `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`,
    time: `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`,
  }
}
