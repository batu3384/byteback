import { describe, expect, it } from 'vitest'
import {
  volumeLetterOfPath,
  normalizeWindowsPath,
  isDestOnScannedDrive,
  isDestOnRaidMemberDrive,
  isDestOnEvidence,
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

  it('blocks dest on scanned drive or RAID members (main-process sync)', () => {
    const resolve = (letter: string) =>
      letter === 'E' ? { driveIndex: 2 } : { driveIndex: 0 }
    expect(isDestOnEvidence('E:\\out', 2, [], resolve)).toBe(true)
    expect(isDestOnEvidence('C:\\out', 2, [], resolve)).toBe(false)
    expect(isDestOnEvidence('E:\\out', -1, [1, 2, 3], resolve)).toBe(true)
    expect(isDestOnEvidence('C:\\out', -1, [1, 2, 3], resolve)).toBe(false)
    expect(isDestOnEvidence('E:\\out', 0, [2], resolve)).toBe(true)
    expect(isDestOnEvidence('E:\\secret.docx', -1, [2], resolve)).toBe(true)
  })

  it('treats dest image file path as the dest volume (imaging)', () => {
    const resolve = (letter: string) =>
      letter === 'E' ? { driveIndex: 2 } : { driveIndex: 0 }
    expect(isDestOnEvidence('E:\\byteback-image.dd', 2, [], resolve)).toBe(true)
    expect(isDestOnEvidence('C:\\byteback-image.dd', 2, [], resolve)).toBe(false)
  })

  it('treats dest spanned diskNumbers as evidence disks', () => {
    const resolve = (letter: string) =>
      letter === 'E' ? { driveIndex: 5, diskNumbers: [5, 6] } : { driveIndex: 0 }
    expect(isDestOnEvidence('E:\\out', 6, [], resolve)).toBe(true)
    expect(isDestOnEvidence('C:\\out', 6, [], resolve)).toBe(false)
  })
})
