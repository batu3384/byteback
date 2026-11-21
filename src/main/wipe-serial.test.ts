import { describe, expect, it } from 'vitest'
import { wipeHitsProtectedVolume, wipeSerialMatches } from './wipe-serial'

describe('wipeSerialMatches', () => {
  it('matches native normalization', () => {
    expect(wipeSerialMatches(' ab c ', 'ABC')).toBe(true)
    expect(wipeSerialMatches('AAA', 'BBB')).toBe(false)
    expect(wipeSerialMatches('', 'X')).toBe(false)
  })
})

describe('wipeHitsProtectedVolume', () => {
  it('refuses the disk that holds the Windows or app-data volume', () => {
    expect(wipeHitsProtectedVolume(0, [{ driveIndex: 0 }])).toBe(true)
    expect(wipeHitsProtectedVolume(2, [{ driveIndex: 0, diskNumbers: [1, 2] }])).toBe(true)
  })
  it('allows other disks and treats an unresolved volume as unknown, not safe', () => {
    expect(wipeHitsProtectedVolume(3, [{ driveIndex: 0 }])).toBe(false)
    expect(wipeHitsProtectedVolume(3, [null])).toBe(true)
  })
})
