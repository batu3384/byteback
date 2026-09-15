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
