import { describe, expect, it } from 'vitest'
import {
  SCAN_PROFILES,
  scanNeedsSsdDeepAck,
  scanProfileLabel,
} from './scan-profiles'
import { scanStepIndex } from './scan-eta'

// The scan profiles are the product's primary mode selector: labels, detail
// text and the SSD-TRIM gate must stay coherent.
describe('scan-profiles', () => {
  it('defines the four profiles with TR labels', () => {
    expect(SCAN_PROFILES.quick.label).toBe('Hızlı')
    expect(SCAN_PROFILES.deep.label).toBe('Derin')
    expect(SCAN_PROFILES.carve_only.label).toContain('carve')
    expect(SCAN_PROFILES.full_carve.label).toContain('Tam disk')
  })

  it('scanNeedsSsdDeepAck covers carve-capable profiles', () => {
    // Carve writes touch TRIM-managed free space: deep, carve_only and
    // full_carve all need the SSD acknowledgment; quick never does.
    expect(scanNeedsSsdDeepAck('quick')).toBe(false)
    expect(scanNeedsSsdDeepAck('deep')).toBe(true)
    expect(scanNeedsSsdDeepAck('carve_only')).toBe(true)
    expect(scanNeedsSsdDeepAck('full_carve')).toBe(true)
  })

  it('scanProfileLabel falls back for unknown types', () => {
    expect(scanProfileLabel('mystery')).toBe('mystery')
  })

  it('scanStepIndex keeps carve_only at carve step', () => {
    const step = scanStepIndex('carve', 'carve_only')
    expect(step.step).toBeGreaterThan(0)
    expect(step.of).toBeGreaterThan(0)
  })
})
