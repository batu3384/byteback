import { describe, expect, it } from 'vitest'
import { ipcOk } from './ipc-result'

describe('ipcOk', () => {
  it('returns ok without error when successful', () => {
    expect(ipcOk(true)).toEqual({ ok: true })
  })

  it('includes error message when provided', () => {
    expect(ipcOk(false, 'busy')).toEqual({ ok: false, error: 'busy' })
  })
})
