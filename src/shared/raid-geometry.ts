/** Stripe sizes the RAID UI offers (matches raid_detect.cpp kStripeSizes). */
export const RAID_STRIPE_BYTES = [65536, 131072, 262144, 524288, 1048576] as const

/** Per-member start offset cap: 1 GiB at 512-byte sectors. */
export const RAID_OFFSET_SECTORS_MAX = 2_097_152

export const RAID_LEVEL_RAID1 = 1
export const RAID_LEVEL_RAID5 = 2

export function isRaidStripeSize(n: unknown): n is number {
  return typeof n === 'number' && Number.isInteger(n) && n >= 512 && n <= 1048576 && (n & (n - 1)) === 0
}

/** RAID1 layout ignores stripe; missing/0 becomes a dummy 64 KiB for the constructor. */
export function raidStripeForReconstruct(raidLevel: number, blockSize: unknown): number | null {
  if (raidLevel === RAID_LEVEL_RAID1 && (blockSize === undefined || blockSize === null || blockSize === 0)) {
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

/** RAID5 P-syndrome scores 1.0 on aligned multiples/divisors; native still reports 1.0. */
export function raid5StripeAmbiguous(raidLevel: number, confidence: number | undefined): boolean {
  return raidLevel === RAID_LEVEL_RAID5 && confidence != null && confidence >= 0.9
}

export function formatRaidStripe(bytes: number): string {
  if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) === 0) return `${bytes / (1024 * 1024)} MiB`
  if (bytes >= 1024 && bytes % 1024 === 0) return `${bytes / 1024} KiB`
  return `${bytes} B`
}
