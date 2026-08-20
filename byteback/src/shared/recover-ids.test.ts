import { describe, it, expect } from 'vitest'
import { parseRecoverIds, parseRecoverIdList } from './recover-ids'

describe('parseRecoverIds', () => {
  it('rejects missing scan or file id so renderer runs cannot be used', () => {
    expect(parseRecoverIds(undefined, 1).ok).toBe(false)
    expect(parseRecoverIds(3, 0).ok).toBe(false)
    expect(parseRecoverIds(3, -1).ok).toBe(false)
  })

  it('accepts positive scan and file ids', () => {
    expect(parseRecoverIds(4, 12)).toEqual({ ok: true, scanId: 4, fileId: 12 })
  })
})

describe('parseRecoverIdList', () => {
  it('rejects a batch without scan id or ids', () => {
    expect(parseRecoverIdList(0, [1]).ok).toBe(false)
    expect(parseRecoverIdList(1, []).ok).toBe(false)
  })

  it('accepts a list of sqlite file ids', () => {
    expect(parseRecoverIdList(2, [10, 11])).toEqual({ ok: true, scanId: 2, fileIds: [10, 11] })
  })
})
