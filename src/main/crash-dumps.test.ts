import { describe, expect, it } from 'vitest'
import { CRASH_DUMPS_DIR_NAME, latestCrashDump } from './crash-dumps'

describe('latestCrashDump', () => {
  it('returns null when no .dmp files exist', () => {
    expect(latestCrashDump([])).toBeNull()
    expect(latestCrashDump([{ name: 'readme.txt', mtimeMs: 9 }])).toBeNull()
  })

  it('picks the newest dump by mtime, not name', () => {
    const got = latestCrashDump([
      { name: 'old.dmp', mtimeMs: 10 },
      { name: 'zzz.dmp', mtimeMs: 5 },
      { name: 'newest.dmp', mtimeMs: 20 },
    ])
    expect(got?.name).toBe('newest.dmp')
    expect(got?.mtimeMs).toBe(20)
  })
})

describe('CRASH_DUMPS_DIR_NAME', () => {
  it('is a local folder name, not a telemetry URL', () => {
    expect(CRASH_DUMPS_DIR_NAME).toBe('CrashDumps')
    expect(CRASH_DUMPS_DIR_NAME).not.toMatch(/^https?:/i)
  })
})
