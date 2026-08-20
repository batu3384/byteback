import { describe, expect, it } from 'vitest'
import {
  canGenerateReport,
  hasValidScanId,
  isScanDependentPage,
  SCAN_DEPENDENT_PAGES,
} from './scan-required'

describe('scan-required', () => {
  it('hasValidScanId rejects non-positive ids', () => {
    expect(hasValidScanId(1)).toBe(true)
    expect(hasValidScanId(0)).toBe(false)
    expect(hasValidScanId(-1)).toBe(false)
  })

  it('canGenerateReport requires valid scan id', () => {
    expect(canGenerateReport(42)).toBe(true)
    expect(canGenerateReport(0)).toBe(false)
  })

  it('marks scan-dependent pages', () => {
    for (const page of SCAN_DEPENDENT_PAGES) {
      expect(isScanDependentPage(page)).toBe(true)
    }
    expect(isScanDependentPage('dashboard')).toBe(false)
    expect(isScanDependentPage('hex')).toBe(false)
  })
})
