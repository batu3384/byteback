/** Last event token after ISO timestamp: `2026-08-21T12:00:00.000Z SCAN_START ...` */
export function parseSessionEvent(line: string): string {
  const parts = line.trim().split(/\s+/)
  return parts.length >= 2 ? parts[1] : ''
}

export function summarizeSessionLines(lines: string[]): string {
  const events = lines.map(parseSessionEvent)
  const start = events.lastIndexOf('SCAN_START')
  if (start < 0) {
    if (events.includes('RENDER_GONE') || events.includes('CRASH')) {
      return 'Uygulama çöktü. Aşağıdaki günlüğe bak.'
    }
    return 'Bu oturumda tarama kaydı yok.'
  }
  const after = events.slice(start)
  if (after.includes('SCAN_COMPLETE')) return 'Son tarama tamamlandı.'
  if (after.includes('SCAN_FAIL')) return 'Son tarama hata ile bitti.'
  if (after.includes('SCAN_STOP')) return 'Son tarama durduruldu (pencere kapandı veya Durdur).'
  const last = events[events.length - 1]
  if (last === 'SCAN_PROGRESS' || last === 'SCAN_START') {
    return 'Tarama sürüyor. Bitene kadar pencereyi kapatma.'
  }
  if (after.includes('SCAN_ORPHAN') || after.includes('APP_START')) {
    return 'Uygulama tarama bitmeden kapandı. Kayıt yarım; Devam et ile sürdürebilirsin.'
  }
  if (after.includes('RENDER_GONE') || after.includes('CRASH')) {
    return 'Tarama sürerken süreç öldü (çökme).'
  }
  if (after.includes('OS_SLEEP')) return 'Tarama sırasında sistem uykuya geçti.'
  if (after.includes('WINDOW_CLOSE') || after.includes('APP_QUIT')) {
    return 'Uygulama tarama bitmeden kapandı. Kayıt yarım; Devam et ile sürdürebilirsin.'
  }
  return 'Son tarama yarıda kaldı. Devam et ile sürdürebilirsin.'
}
