import { describe, expect, it } from 'vitest'
import { volumeLetterOfPath, isDestOnScannedDrive } from './recover-dest-guard'

describe('recover-dest-guard', () => {
  it('parses volume letter from Windows paths', () => {
    expect(volumeLetterOfPath('D:\\byteback-out')).toBe('D')
    expect(volumeLetterOfPath('e:/tmp')).toBe('E')
    expect(volumeLetterOfPath('/mnt/data')).toBeNull()
  })

  it('detects dest on same physical drive', async () => {
    const resolve = async (letter: string) =>
      letter === 'E' ? { driveIndex: 2 } : { driveIndex: 0 }
    expect(await isDestOnScannedDrive('E:\\out', 2, resolve)).toBe(true)
    expect(await isDestOnScannedDrive('C:\\out', 2, resolve)).toBe(false)
    expect(await isDestOnScannedDrive('E:\\out', -1, resolve)).toBe(false)
  })
})
