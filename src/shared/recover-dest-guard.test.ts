import { describe, expect, it } from 'vitest'
import {
  volumeLetterOfPath,
  normalizeWindowsPath,
  isDestOnScannedDrive,
  isDestOnRaidMemberDrive,
} from './recover-dest-guard'

describe('recover-dest-guard', () => {
  it('normalizes extended Win32 paths', () => {
    expect(normalizeWindowsPath('\\\\?\\D:\\byteback-out')).toBe('D:\\byteback-out')
    expect(normalizeWindowsPath('\\\\?\\UNC\\server\\share\\out')).toBe('\\\\server\\share\\out')
  })

  it('parses volume letter from Windows paths', () => {
    expect(volumeLetterOfPath('D:\\byteback-out')).toBe('D')
    expect(volumeLetterOfPath('e:/tmp')).toBe('E')
    expect(volumeLetterOfPath('\\\\?\\D:\\out')).toBe('D')
    expect(volumeLetterOfPath('/mnt/data')).toBeNull()
    expect(volumeLetterOfPath('\\\\server\\share\\out')).toBeNull()
  })

  it('detects dest on same physical drive', async () => {
    const resolve = async (letter: string) =>
      letter === 'E' ? { driveIndex: 2 } : { driveIndex: 0 }
    expect(await isDestOnScannedDrive('E:\\out', 2, resolve)).toBe(true)
    expect(await isDestOnScannedDrive('C:\\out', 2, resolve)).toBe(false)
    expect(await isDestOnScannedDrive('E:\\out', -1, resolve)).toBe(false)
    expect(await isDestOnScannedDrive('\\\\?\\E:\\out', 2, resolve)).toBe(true)
  })

  it('detects dest on RAID member drive', async () => {
    const resolve = async (letter: string) =>
      letter === 'E' ? { driveIndex: 2 } : { driveIndex: 0 }
    expect(await isDestOnRaidMemberDrive('E:\\out', [1, 2, 3], resolve)).toBe(true)
    expect(await isDestOnRaidMemberDrive('C:\\out', [1, 2, 3], resolve)).toBe(false)
    expect(await isDestOnRaidMemberDrive('E:\\out', [], resolve)).toBe(false)
  })
})
