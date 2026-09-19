import type { RaidState } from './ipc-contract'

export interface SectorReadResult {
  success: boolean
  paddedZeros?: boolean
  data?: ArrayLike<number> | null
}

/** Unread RAID state is not inactive — dest-on-member and source pick must not assume no array. */
export type RaidStateProbe =
  | { status: 'ok'; state: RaidState }
  | { status: 'unread' }

export async function probeRaidState(
  getRaidState?: () => Promise<RaidState>,
): Promise<RaidStateProbe> {
  if (!getRaidState) return { status: 'unread' }
  try {
    const s = await getRaidState()
    if (!s || typeof s.active !== 'boolean') return { status: 'unread' }
    return { status: 'ok', state: s }
  } catch {
    return { status: 'unread' }
  }
}

/** Same sentinel as start-scan: assembled RAID view, not PhysicalDrive -1. */
export const HEX_RAID_DRIVE_INDEX = -1

/** Local evidence image scan (drivePath "image"), not PhysicalDrive -2. */
export const SCAN_IMAGE_DRIVE_INDEX = -2

export function isHexDriveIndex(v: unknown): v is number {
  return typeof v === 'number' && Number.isInteger(v)
    && (v === HEX_RAID_DRIVE_INDEX || v === SCAN_IMAGE_DRIVE_INDEX || v >= 0)
}

/** RAID assembly is opt-in. A live array must not hijack member/volume hex. */
export function hexUsesRaidBackend(raidActive: boolean, driveIndex: number): boolean {
  return raidActive && driveIndex === HEX_RAID_DRIVE_INDEX
}

/** Hex UI shows a sector only when the read is complete and not zero-padded. */
export function hexDataOrNull(res: SectorReadResult | null | undefined): number[] | null {
  if (!res || !res.success || res.paddedZeros || !res.data) return null
  const out: number[] = []
  const data = res.data
  const n = data.length
  if (n <= 0) return null
  for (let i = 0; i < n; i++) out.push(data[i])
  return out
}

/** Keep in sync with kHexSearchMaxNeedle in hex_search.h. */
export const HEX_SEARCH_MAX_NEEDLE = 64
export const HEX_SEARCH_MAX_HITS = 256

export type HexNeedleParse =
  | { ok: true; bytes: number[] }
  | { ok: false; reason: 'empty' | 'too_long' }

function latin1Bytes(s: string): number[] {
  const out: number[] = []
  for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i) & 0xff)
  return out
}

/** Even hex digits (spaces / 0x allowed) win; otherwise latin1 text. Cap 64 bytes. */
export function parseHexSearchNeedle(query: string): HexNeedleParse {
  if (typeof query !== 'string') return { ok: false, reason: 'empty' }
  const trimmed = query.trim()
  if (!trimmed) return { ok: false, reason: 'empty' }
  let hexBody = trimmed.replace(/\s+/g, '')
  if (/^0x/i.test(hexBody)) hexBody = hexBody.slice(2)
  if (hexBody.length > 0 && hexBody.length % 2 === 0 && /^[0-9a-fA-F]+$/.test(hexBody)) {
    const bytes: number[] = []
    for (let i = 0; i < hexBody.length; i += 2) {
      bytes.push(parseInt(hexBody.slice(i, i + 2), 16))
    }
    if (bytes.length > HEX_SEARCH_MAX_NEEDLE) return { ok: false, reason: 'too_long' }
    return { ok: true, bytes }
  }
  const text = latin1Bytes(trimmed)
  if (text.length > HEX_SEARCH_MAX_NEEDLE) return { ok: false, reason: 'too_long' }
  return { ok: true, bytes: text }
}

export function hexSearchHitSector(byteOffset: number, sectorSize: number): number {
  if (!Number.isFinite(byteOffset) || byteOffset < 0) return 0
  const ss = Number.isFinite(sectorSize) && sectorSize > 0 ? Math.floor(sectorSize) : 512
  return Math.floor(byteOffset / ss)
}

const MFT_ATTR_NAMES: Record<number, string> = {
  0x10: '$STANDARD_INFORMATION',
  0x20: '$ATTRIBUTE_LIST',
  0x30: '$FILE_NAME',
  0x40: '$OBJECT_ID',
  0x50: '$SECURITY_DESCRIPTOR',
  0x60: '$VOLUME_NAME',
  0x70: '$VOLUME_INFORMATION',
  0x80: '$DATA',
  0x90: '$INDEX_ROOT',
  0xa0: '$INDEX_ALLOCATION',
  0xb0: '$BITMAP',
  0xc0: '$REPARSE_POINT',
}

export function mftAttrTypeLabel(type: number): string {
  return MFT_ATTR_NAMES[type] ?? `0x${type.toString(16).toUpperCase()}`
}
