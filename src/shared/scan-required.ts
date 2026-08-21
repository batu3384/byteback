/** Pages and actions that require a completed scan persisted in SQLite. */

export const SCAN_DEPENDENT_PAGES = ['results', 'search', 'timeline', 'report'] as const
export const DISK_BUSY_PAGES = ['hex', 'imager', 'shredder'] as const

export type ScanDependentPage = (typeof SCAN_DEPENDENT_PAGES)[number]

export function isScanDependentPage(page: string): page is ScanDependentPage {
  return (SCAN_DEPENDENT_PAGES as readonly string[]).includes(page)
}

export type DiskBusyPage = (typeof DISK_BUSY_PAGES)[number]

export function isDiskBusyPage(page: string): page is DiskBusyPage {
  return (DISK_BUSY_PAGES as readonly string[]).includes(page)
}

export function hasValidScanId(scanId: number): boolean {
  return typeof scanId === 'number' && scanId > 0
}

export function canGenerateReport(scanId: number): boolean {
  return hasValidScanId(scanId)
}

export function isLiveScanStatus(status: string): boolean {
  return status === 'Tarama Sürüyor...'
    || status === 'Tarama Devam Ediyor...'
    || status === 'RAID Taraması Sürüyor...'
    || status === 'Durduruluyor...'
}

export function diskBusyMessage(raw?: string): string | undefined {
  if (!raw) return undefined
  if (raw.includes('Another disk operation is already running')) {
    return 'Tarama veya başka disk işlemi sürüyor. Bu okuma tarama bitince kullanılabilir.'
  }
  return raw
}
