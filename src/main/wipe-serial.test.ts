import { describe, expect, it } from 'vitest'
import { wipeSerialMatches } from './wipe-serial'

describe('wipeSerialMatches', () => {
  it('matches native normalization', () => {
    expect(wipeSerialMatches(' ab c ', 'ABC')).toBe(true)
    expect(wipeSerialMatches('AAA', 'BBB')).toBe(false)
    expect(wipeSerialMatches('', 'X')).toBe(false)
  })
})
