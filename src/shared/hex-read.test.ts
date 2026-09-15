import { describe, it, expect } from 'vitest'
import { hexDataOrNull, hexUsesRaidBackend, isHexDriveIndex, HEX_RAID_DRIVE_INDEX, probeRaidState } from './hex-read'

describe('hexDataOrNull', () => {
  it('returns bytes for a clean read', () => {
    expect(hexDataOrNull({ success: true, data: [1, 2, 3] })).toEqual([1, 2, 3])
  })

  it('returns null when the read was zero-padded', () => {
    expect(hexDataOrNull({ success: true, paddedZeros: true, data: [0, 0] })).toBeNull()
  })

  it('returns null when success is false', () => {
    expect(hexDataOrNull({ success: false, data: [1] })).toBeNull()
  })
})

describe('hex drive bind', () => {
  it('allows the RAID sentinel and non-negative integers only', () => {
    expect(isHexDriveIndex(HEX_RAID_DRIVE_INDEX)).toBe(true)
    expect(isHexDriveIndex(0)).toBe(true)
    expect(isHexDriveIndex(12)).toBe(true)
    expect(isHexDriveIndex(-2)).toBe(false)
    expect(isHexDriveIndex(1.5)).toBe(false)
    expect(isHexDriveIndex('0')).toBe(false)
  })

  it('does not route member hex through a live RAID assembly', () => {
    expect(hexUsesRaidBackend(true, HEX_RAID_DRIVE_INDEX)).toBe(true)
    expect(hexUsesRaidBackend(true, 0)).toBe(false)
    expect(hexUsesRaidBackend(false, HEX_RAID_DRIVE_INDEX)).toBe(false)
  })
})

describe('probeRaidState', () => {
  const inactive = { active: false, capacity: 0, numDisks: 0, level: -1, memberDriveIndices: [] as number[] }

  it('does not treat a missing getter as an inactive array', async () => {
    expect(await probeRaidState(undefined)).toEqual({ status: 'unread' })
  })

  it('does not treat a throw as inactive', async () => {
    expect(await probeRaidState(async () => { throw new Error('engine down') })).toEqual({ status: 'unread' })
  })

  it('does not treat a shapeless payload as inactive', async () => {
    expect(await probeRaidState(async () => ({}) as never)).toEqual({ status: 'unread' })
  })

  it('returns the live state when the probe succeeds', async () => {
    expect(await probeRaidState(async () => inactive)).toEqual({ status: 'ok', state: inactive })
  })
})
