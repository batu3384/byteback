import { isEvidenceImagePath } from './win32-volume-path'

/** Stripe sizes the RAID UI offers (matches raid_detect.cpp kStripeSizes). */
export const RAID_STRIPE_BYTES = [65536, 131072, 262144, 524288, 1048576] as const

/** Per-member start offset cap: 1 GiB at 512-byte sectors. */
export const RAID_OFFSET_SECTORS_MAX = 2_097_152

export const RAID_LEVEL_RAID1 = 1
export const RAID_LEVEL_RAID5 = 2
export const RAID_LEVEL_JBOD = 5
export const RAID_LEVEL_RAID1E = 6
export const RAID5_ALGO_LEFT_ASYMMETRIC = 0
export const RAID5_ALGO_LEFT_SYMMETRIC = 1
export const RAID5_ALGO_RIGHT_ASYMMETRIC = 2
export const RAID5_ALGO_RIGHT_SYMMETRIC = 3

export function isRaidStripeSize(n: unknown): n is number {
  return typeof n === 'number' && Number.isInteger(n) && n >= 512 && n <= 1048576 && (n & (n - 1)) === 0
}

/** RAID1/JBOD layout ignores stripe; missing/0 becomes a dummy 64 KiB for the constructor. */
export function raidStripeForReconstruct(raidLevel: number, blockSize: unknown): number | null {
  if ((raidLevel === RAID_LEVEL_RAID1 || raidLevel === RAID_LEVEL_JBOD) &&
      (blockSize === undefined || blockSize === null || blockSize === 0)) {
    return 65536
  }
  if (!isRaidStripeSize(blockSize)) return null
  return blockSize
}

export function raidOffsetSectorsForReconstruct(v: unknown): number | null {
  if (v === undefined || v === null) return 0
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 0 || v > RAID_OFFSET_SECTORS_MAX) return null
  return v
}

/** RAID5 P-syndrome can tie aligned sizes; FS magics pin the stripe when present. */
export function raid5StripeAmbiguous(
  raidLevel: number,
  confidence: number | undefined,
  fsConfirmed?: boolean,
): boolean {
  if (fsConfirmed) return false
  return raidLevel === RAID_LEVEL_RAID5 && confidence != null && confidence >= 0.9
}

export function formatRaidStripe(bytes: number): string {
  if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) === 0) return `${bytes / (1024 * 1024)} MiB`
  if (bytes >= 1024 && bytes % 1024 === 0) return `${bytes / 1024} KiB`
  return `${bytes} B`
}

/** Drive indices or evidence files — never mixed. Bare digits are drives, not files. */
export function raidAssembleRequest(
  ids: string[],
): { kind: 'drive'; indices: number[] } | { kind: 'image'; paths: string[] } | null {
  if (ids.length < 2) return null
  if (ids.every((id) => /^\d+$/.test(id))) {
    return { kind: 'drive', indices: ids.map((id) => Number(id)) }
  }
  if (ids.every((id) => !/^\d+$/.test(id) && isEvidenceImagePath(id))) {
    return { kind: 'image', paths: ids }
  }
  return null
}

/** IPC channel for auto-detect. Mix/null stays unsupported (honest fail). */
export function raidDetectChannel(ids: string[]): 'detect-raid' | 'detect-raid-images' | null {
  const req = raidAssembleRequest(ids)
  if (!req) return null
  return req.kind === 'drive' ? 'detect-raid' : 'detect-raid-images'
}

/** Reorder RAID members by detect permutation. Null if order is not a permutation of [0..n). */
export function applyRaidMemberOrder<T>(items: T[], order: unknown): T[] | null {
  if (!Array.isArray(order) || order.length !== items.length) return null
  const seen = new Set<number>()
  const out: T[] = []
  for (const i of order) {
    if (typeof i !== 'number' || !Number.isInteger(i) || i < 0 || i >= items.length || seen.has(i)) {
      return null
    }
    seen.add(i)
    out.push(items[i])
  }
  return out
}

/** 0..3 = left-asym, left-sym (mdadm), right-asym, right-sym. Missing becomes 0. */
export function raid5AlgorithmForReconstruct(v: unknown): number | null {
  if (v === undefined || v === null) return RAID5_ALGO_LEFT_ASYMMETRIC
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 0 || v > 3) return null
  return v
}
