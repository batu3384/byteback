import { describe, it, expect } from 'vitest'
import { loadAllowedImageDest, saveAllowedImageDest } from './image-dest-allowlist'
import { mkdtempSync, rmSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'

describe('image dest allowlist persist', () => {
  it('round-trips picked destinations across process restart', () => {
    const dir = mkdtempSync(join(tmpdir(), 'byteback-allow-'))
    const file = join(dir, 'allowed-image-dest.json')
    const set = new Set(['C:\\\\img\\\\disk.dd'])
    saveAllowedImageDest(file, set)
    const loaded = loadAllowedImageDest(file)
    expect(loaded.has('C:\\\\img\\\\disk.dd')).toBe(true)
    rmSync(dir, { recursive: true, force: true })
  })

  it('returns empty set when the file is missing', () => {
    expect(loadAllowedImageDest(join(tmpdir(), 'byteback-missing-allowlist.json')).size).toBe(0)
  })
})
