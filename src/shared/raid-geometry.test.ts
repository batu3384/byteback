import { describe, expect, it } from 'vitest'
import {
  formatRaidStripe,
  isRaidStripeSize,
  raid5StripeAmbiguous,
  raidOffsetSectorsForReconstruct,
  raidStripeForReconstruct,
  RAID_LEVEL_RAID1,
  RAID_LEVEL_RAID5,
} from './raid-geometry'

describe('raid-geometry', () => {
  it('accepts power-of-two stripes from 512 B to 1 MiB', () => {
    expect(isRaidStripeSize(512)).toBe(true)
    expect(isRaidStripeSize(4096)).toBe(true)
    expect(isRaidStripeSize(65536)).toBe(true)
    expect(isRaidStripeSize(1048576)).toBe(true)
    expect(isRaidStripeSize(0)).toBe(false)
    expect(isRaidStripeSize(65535)).toBe(false)
    expect(isRaidStripeSize(2 * 1048576)).toBe(false)
    expect(isRaidStripeSize(65536.5)).toBe(false)
  })

  it('does not silently default a striped reconstruct to 64 KiB', () => {
    expect(raidStripeForReconstruct(0, undefined)).toBeNull()
    expect(raidStripeForReconstruct(RAID_LEVEL_RAID5, 0)).toBeNull()
    expect(raidStripeForReconstruct(RAID_LEVEL_RAID5, 65536)).toBe(65536)
    expect(raidStripeForReconstruct(RAID_LEVEL_RAID1, undefined)).toBe(65536)
    expect(raidStripeForReconstruct(RAID_LEVEL_RAID1, 0)).toBe(65536)
  })

  it('rejects a non-integer or oversized member offset', () => {
    expect(raidOffsetSectorsForReconstruct(undefined)).toBe(0)
    expect(raidOffsetSectorsForReconstruct(2048)).toBe(2048)
    expect(raidOffsetSectorsForReconstruct(-1)).toBeNull()
    expect(raidOffsetSectorsForReconstruct(1.5)).toBeNull()
    expect(raidOffsetSectorsForReconstruct(2_097_153)).toBeNull()
  })

  it('flags RAID5 high-confidence P-syndrome as stripe-ambiguous, not RAID6', () => {
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 1)).toBe(true)
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 0.5)).toBe(false)
    expect(raid5StripeAmbiguous(3, 1)).toBe(false)
  })

  it('formats stripe sizes in binary units', () => {
    expect(formatRaidStripe(65536)).toBe('64 KiB')
    expect(formatRaidStripe(1048576)).toBe('1 MiB')
  })
})
