/** Last event token after ISO timestamp: `2026-08-21T12:00:00.000Z SCAN_START ...` */
export function parseSessionEvent(line: string): string {
  const parts = line.trim().split(/\s+/)
  return parts.length >= 2 ? parts[1] : ''
}

// Machine-readable classification of the last session, so the renderer never
// has to match Turkish substrings of the human summary.
export type SessionLogCode =
  | 'no_scan'
  | 'crash'
  | 'complete'
  | 'fail'
  | 'stopped'
  | 'running'
  | 'quit_early'
  | 'crash_during_scan'
  | 'sleep'
  | 'incomplete'

export function sessionLogCode(lines: string[]): SessionLogCode {
  const events = lines.map(parseSessionEvent)
  const start = events.lastIndexOf('SCAN_START')
  if (start < 0) {
    if (events.includes('RENDER_GONE') || events.includes('CRASH')) return 'crash'
    return 'no_scan'
  }
  const after = events.slice(start)
  if (after.includes('SCAN_COMPLETE')) return 'complete'
  if (after.includes('SCAN_FAIL')) return 'fail'
  if (after.includes('SCAN_STOP')) return 'stopped'
  const last = events[events.length - 1]
  if (last === 'SCAN_PROGRESS' || last === 'SCAN_START') return 'running'
  if (after.includes('RENDER_GONE') || after.includes('CRASH')) return 'crash_during_scan'
  if (after.includes('OS_SLEEP')) return 'sleep'
  if (after.includes('SCAN_ORPHAN') || after.includes('APP_START') ||
      after.includes('WINDOW_CLOSE') || after.includes('APP_QUIT')) return 'quit_early'
  return 'incomplete'
}

const SUMMARY_TR: Record<SessionLogCode, string> = {
  no_scan: 'Bu oturumda tarama kaydı yok.',
  crash: 'Uygulama çöktü. Aşağıdaki günlüğe bak.',
  complete: 'Son tarama tamamlandı.',
  fail: 'Son tarama hata ile bitti.',
  stopped: 'Son tarama durduruldu (pencere kapandı veya Durdur).',
  running: 'Tarama sürüyor. Bitene kadar pencereyi kapatma.',
  crash_during_scan: 'Tarama sürerken süreç öldü (çökme).',
  sleep: 'Tarama sırasında sistem uykuya geçti.',
  quit_early: 'Uygulama tarama bitmeden kapandı. Kayıt yarım; Devam et ile sürdürebilirsin.',
  incomplete: 'Son tarama yarıda kaldı. Devam et ile sürdürebilirsin.',
}

export function summarizeSessionLines(lines: string[]): string {
  return SUMMARY_TR[sessionLogCode(lines)]
}
