import { describe, expect, it } from 'vitest'
import {
  canGenerateReport,
  diskBusyMessage,
  hasValidScanId,
  isDiskBusyPage,
  isLiveScanStatus,
  isScanDependentPage,
  SCAN_DEPENDENT_PAGES,
} from './scan-required'

describe('scan-required', () => {
  it('hasValidScanId rejects non-positive ids', () => {
    expect(hasValidScanId(1)).toBe(true)
    expect(hasValidScanId(0)).toBe(false)
    expect(hasValidScanId(-1)).toBe(false)
  })

  it('canGenerateReport requires completed scan', () => {
    expect(canGenerateReport(42, { id: 42, driveIndex: 0, scanType: 'deep', totalSectors: 1, scannedSectors: 1, status: 1 })).toBe(true)
    expect(canGenerateReport(42, { id: 42, driveIndex: 0, scanType: 'deep', totalSectors: 1, scannedSectors: 1, status: 4 })).toBe(false)
    expect(canGenerateReport(0)).toBe(false)
  })

  it('marks scan-dependent pages', () => {
    for (const page of SCAN_DEPENDENT_PAGES) {
      expect(isScanDependentPage(page)).toBe(true)
    }
    expect(isScanDependentPage('dashboard')).toBe(false)
    expect(isScanDependentPage('hex')).toBe(false)
  })

  it('marks disk-busy pages that must wait out a live scan', () => {
    expect(isDiskBusyPage('hex')).toBe(true)
    expect(isDiskBusyPage('imager')).toBe(true)
    expect(isDiskBusyPage('shredder')).toBe(true)
    expect(isDiskBusyPage('dashboard')).toBe(false)
  })

  it('isLiveScanStatus only while a scan is in flight', () => {
    expect(isLiveScanStatus('Tarama Sürüyor...')).toBe(true)
    expect(isLiveScanStatus('RAID Taraması Sürüyor...')).toBe(true)
    expect(isLiveScanStatus('Tarama Tamamlandı')).toBe(false)
    expect(isLiveScanStatus('Bekleniyor...')).toBe(false)
  })

  it('diskBusyMessage translates native busy errors', () => {
    expect(diskBusyMessage('Another disk operation is already running')?.includes('önizleme')).toBe(true)
    expect(diskBusyMessage('Sektör okunamadı')).toBe('Sektör okunamadı')
  })
})
