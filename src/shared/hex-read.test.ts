import { describe, it, expect } from 'vitest'
import {
  hexDataOrNull,
  hexUsesRaidBackend,
  isHexDriveIndex,
  HEX_RAID_DRIVE_INDEX,
  probeRaidState,
  parseHexSearchNeedle,
  hexSearchHitSector,
  HEX_SEARCH_MAX_NEEDLE,
  mftAttrTypeLabel,
} from './hex-read'

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
  it('allows the RAID sentinel, image sentinel, and non-negative integers', () => {
    expect(isHexDriveIndex(HEX_RAID_DRIVE_INDEX)).toBe(true)
    expect(isHexDriveIndex(0)).toBe(true)
    expect(isHexDriveIndex(12)).toBe(true)
    expect(isHexDriveIndex(-2)).toBe(true)
    expect(isHexDriveIndex(-3)).toBe(false)
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

describe('parseHexSearchNeedle', () => {
  it('parses even hex digits including spaces and 0x', () => {
    expect(parseHexSearchNeedle('DEADBEEF')).toEqual({ ok: true, bytes: [0xde, 0xad, 0xbe, 0xef] })
    expect(parseHexSearchNeedle('de ad be ef')).toEqual({ ok: true, bytes: [0xde, 0xad, 0xbe, 0xef] })
    expect(parseHexSearchNeedle('0x46494C45')).toEqual({ ok: true, bytes: [0x46, 0x49, 0x4c, 0x45] })
  })

  it('treats non-hex text as latin1 bytes', () => {
    expect(parseHexSearchNeedle('FILE')).toEqual({ ok: true, bytes: [0x46, 0x49, 0x4c, 0x45] })
  })

  it('rejects empty and over-cap needles', () => {
    expect(parseHexSearchNeedle('')).toEqual({ ok: false, reason: 'empty' })
    expect(parseHexSearchNeedle('   ')).toEqual({ ok: false, reason: 'empty' })
    const tooLong = 'aa'.repeat(HEX_SEARCH_MAX_NEEDLE + 1)
    expect(parseHexSearchNeedle(tooLong)).toEqual({ ok: false, reason: 'too_long' })
    const longText = 'x'.repeat(HEX_SEARCH_MAX_NEEDLE + 1)
    expect(parseHexSearchNeedle(longText)).toEqual({ ok: false, reason: 'too_long' })
  })
})

describe('hexSearchHitSector', () => {
  it('maps a byte offset onto the containing sector', () => {
    expect(hexSearchHitSector(1000, 512)).toBe(1)
    expect(hexSearchHitSector(512, 512)).toBe(1)
    expect(hexSearchHitSector(0, 512)).toBe(0)
  })
})

describe('mftAttrTypeLabel', () => {
  it('names well-known NTFS attribute types', () => {
    expect(mftAttrTypeLabel(0x80)).toBe('$DATA')
    expect(mftAttrTypeLabel(0x30)).toBe('$FILE_NAME')
    expect(mftAttrTypeLabel(0x10)).toBe('$STANDARD_INFORMATION')
  })
})
