import { describe, it, expect } from 'vitest'
import { appErrorBoundaryCopy } from './app-error-boundary-message'

describe('appErrorBoundaryCopy', () => {
  it('asks for window reload while scan is live', () => {
    const c = appErrorBoundaryCopy(true, 42)
    expect(c.useWindowReload).toBe(true)
    expect(c.primaryLabel).toMatch(/reload/i)
  })

  it('shows last scan id when scan is not live', () => {
    const c = appErrorBoundaryCopy(false, 7)
    expect(c.body).toContain('7')
    expect(c.useWindowReload).toBe(false)
  })
})
