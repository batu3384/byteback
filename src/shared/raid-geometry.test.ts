import { describe, expect, it } from 'vitest'
import {
  formatRaidStripe,
  isRaidStripeSize,
  raid5StripeAmbiguous,
  raidOffsetSectorsForReconstruct,
  raidStripeForReconstruct,
  RAID_LEVEL_RAID1,
  RAID_LEVEL_RAID5,
  raidAssembleRequest,
  raidDetectChannel,
  applyRaidMemberOrder,
  raid5AlgorithmForReconstruct,
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
    expect(raidStripeForReconstruct(5, 0)).toBe(65536)
    expect(raidStripeForReconstruct(6, 0)).toBeNull()
    expect(raidStripeForReconstruct(6, 65536)).toBe(65536)
  })

  it('rejects a non-integer or oversized member offset', () => {
    expect(raidOffsetSectorsForReconstruct(undefined)).toBe(0)
    expect(raidOffsetSectorsForReconstruct(2048)).toBe(2048)
    expect(raidOffsetSectorsForReconstruct(-1)).toBeNull()
    expect(raidOffsetSectorsForReconstruct(1.5)).toBeNull()
    expect(raidOffsetSectorsForReconstruct(2_097_153)).toBeNull()
  })

  it('flags RAID5 high-confidence P-syndrome as stripe-ambiguous unless FS-confirmed', () => {
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 1)).toBe(true)
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 1, false)).toBe(true)
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 1, true)).toBe(false)
    expect(raid5StripeAmbiguous(RAID_LEVEL_RAID5, 0.5)).toBe(false)
    expect(raid5StripeAmbiguous(3, 1)).toBe(false)
  })

  it('formats stripe sizes in binary units', () => {
    expect(formatRaidStripe(65536)).toBe('64 KiB')
    expect(formatRaidStripe(1048576)).toBe('1 MiB')
  })

  it('assembles from drive indices or evidence image paths, not mixed', () => {
    expect(raidAssembleRequest(['0', '1'])).toEqual({ kind: 'drive', indices: [0, 1] })
    expect(raidAssembleRequest(['C:\\a.img', 'D:\\b.img'])).toEqual({
      kind: 'image',
      paths: ['C:\\a.img', 'D:\\b.img'],
    })
    expect(raidAssembleRequest(['0', 'C:\\a.img'])).toBeNull()
    expect(raidAssembleRequest(['0'])).toBeNull()
    expect(raidAssembleRequest(['\\\\.\\PhysicalDrive0', 'C:\\a.img'])).toBeNull()
  })

  it('routes auto-detect to image IPC when members are evidence files', () => {
    expect(raidDetectChannel(['C:\\a.img', 'D:\\b.img'])).toBe('detect-raid-images')
    expect(raidDetectChannel(['0', '1'])).toBe('detect-raid')
    expect(raidDetectChannel(['0', 'C:\\a.img'])).toBeNull()
  })

  it('applies a permutation of RAID members and rejects bogus order', () => {
    expect(applyRaidMemberOrder(['a', 'b', 'c'], [2, 0, 1])).toEqual(['c', 'a', 'b'])
    expect(applyRaidMemberOrder(['a', 'b', 'c'], [0, 1, 2])).toEqual(['a', 'b', 'c'])
    expect(applyRaidMemberOrder(['a', 'b'], [0, 0])).toBeNull()
    expect(applyRaidMemberOrder(['a', 'b'], [0])).toBeNull()
    expect(applyRaidMemberOrder(['a', 'b'], [0, 2])).toBeNull()
  })

  it('accepts RAID5 left-asymmetric default and left-symmetric mdadm', () => {
    expect(raid5AlgorithmForReconstruct(undefined)).toBe(0)
    expect(raid5AlgorithmForReconstruct(1)).toBe(1)
    expect(raid5AlgorithmForReconstruct(0)).toBe(0)
    expect(raid5AlgorithmForReconstruct(2)).toBe(2)
    expect(raid5AlgorithmForReconstruct(3)).toBe(3)
    expect(raid5AlgorithmForReconstruct(4)).toBeNull()
    expect(raid5AlgorithmForReconstruct(-1)).toBeNull()
  })
})
