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

export function isHexDriveIndex(v: unknown): v is number {
  return typeof v === 'number' && Number.isInteger(v) && (v === HEX_RAID_DRIVE_INDEX || v >= 0)
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
