/** Pages and actions that require a completed scan persisted in SQLite. */

export const SCAN_DEPENDENT_PAGES = ['results', 'search', 'timeline', 'report'] as const

export type ScanDependentPage = (typeof SCAN_DEPENDENT_PAGES)[number]

export function isScanDependentPage(page: string): page is ScanDependentPage {
  return (SCAN_DEPENDENT_PAGES as readonly string[]).includes(page)
}

export function hasValidScanId(scanId: number): boolean {
  return typeof scanId === 'number' && scanId > 0
}

export function canGenerateReport(scanId: number): boolean {
  return hasValidScanId(scanId)
}
