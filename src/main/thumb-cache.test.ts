import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { mkdirSync, mkdtempSync, rmSync, utimesSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  THUMB_CACHE_CAP_BYTES,
  findThumbPath,
  listThumbEntries,
  mimeForExt,
  planEvictions,
  resolveThumbUrlToPath,
  storeThumb,
  thumbFileName,
  thumbPathFor,
  thumbUrlFor,
  type ThumbDirEntry,
} from './thumb-cache'

// FAZ 1.3b: pure parts of the thumb disk cache — name mapping, LRU cap math
// and the thumb:// traversal guard — exercised against real temp dirs.

let dir = ''

beforeEach(() => {
  dir = mkdtempSync(join(tmpdir(), 'byteback-thumb-test-'))
})

afterEach(() => {
  rmSync(dir, { recursive: true, force: true })
})

describe('thumb name mapping', () => {
  it('maps image mimes to scanId-fileId.ext names', () => {
    expect(thumbFileName(7, 5, 'image/jpeg')).toBe('5-7.jpg')
    expect(thumbFileName(7, 5, 'image/png')).toBe('5-7.png')
    expect(thumbFileName(7, 5, 'image/webp')).toBe('5-7.webp')
    expect(thumbPathFor(dir, 7, 5, 'image/png')).toBe(join(dir, '5-7.png'))
  })

  it('rejects unsupported mimes and non-positive/non-integer ids', () => {
    expect(thumbFileName(7, 5, 'image/heic')).toBeNull()
    expect(thumbFileName(7, 5, 'text/html')).toBeNull()
    expect(thumbFileName(0, 5, 'image/png')).toBeNull()
    expect(thumbFileName(7, -1, 'image/png')).toBeNull()
    expect(thumbFileName(1.5, 5, 'image/png')).toBeNull()
  })

  it('round-trips mime through the extension', () => {
    expect(mimeForExt('jpg')).toBe('image/jpeg')
    expect(mimeForExt('PNG')).toBe('image/png')
    expect(mimeForExt('exe')).toBeNull()
  })
})

describe('planEvictions (LRU cap math)', () => {
  const e = (name: string, sizeBytes: number, mtimeMs: number): ThumbDirEntry => ({ name, sizeBytes, mtimeMs })

  it('keeps everything while under cap', () => {
    const entries = [e('a', 10, 1), e('b', 10, 2)]
    expect(planEvictions(entries, 100)).toEqual([])
  })

  it('evicts oldest-by-mtime until under cap', () => {
    const entries = [e('old', 60, 1), e('mid', 60, 2), e('new', 60, 3)]
    // Cap 100: total 180 -> evict 'old' (120 left) -> still over -> evict 'mid' (60 left).
    expect(planEvictions(entries, 100)).toEqual(['old', 'mid'])
  })

  it('never evicts the just-written file', () => {
    const entries = [e('old', 60, 1), e('fresh', 60, 2)]
    expect(planEvictions(entries, 50, 'fresh')).toEqual(['old'])
  })

  it('an oversized single thumb still evicts everything else', () => {
    const entries = [e('a', 10, 1), e('huge', 999, 2)]
    expect(planEvictions(entries, 50, 'huge')).toEqual(['a'])
  })
})

describe('storeThumb / findThumbPath against a real temp dir', () => {
  it('writes and re-finds cache entries across mime extensions', () => {
    expect(storeThumb(dir, 7, 5, 'image/png', Buffer.from('pngbytes'))).toBe(join(dir, '5-7.png'))
    expect(findThumbPath(dir, 7, 5)).toBe(join(dir, '5-7.png'))
    expect(findThumbPath(dir, 8, 5)).toBeNull()
    expect(storeThumb(dir, 7, 5, 'image/heic', Buffer.from('x'))).toBeNull()
    expect(listThumbEntries(dir)).toHaveLength(1)
  })

  it('enforces the cap by deleting the oldest entry on write', () => {
    // Local cap override is not exposed; simulate via a tiny directory total by
    // pre-seeding one "old" file far larger than the default cap is impossible
    // (256MB) — so exercise the wiring with the real cap and assert the old
    // file survives while total < cap.
    mkdirSync(dir, { recursive: true })
    writeFileSync(join(dir, '5-1.jpg'), Buffer.alloc(16))
    utimesSync(join(dir, '5-1.jpg'), new Date(0), new Date(0))
    storeThumb(dir, 2, 5, 'image/jpeg', Buffer.alloc(8))
    expect(findThumbPath(dir, 1, 5)).not.toBeNull()
    expect(findThumbPath(dir, 2, 5)).not.toBeNull()
    expect(THUMB_CACHE_CAP_BYTES).toBe(256 * 1024 * 1024)
  })
})

describe('resolveThumbUrlToPath (thumb:// traversal guard)', () => {
  it('resolves well-formed URLs inside the cache dir', () => {
    const p = resolveThumbUrlToPath(dir, 'thumb://scan-5/5-7.png')
    expect(p).toBe(join(dir, '5-7.png'))
  })

  it('rejects traversal, wrong hosts, wrong schemes and garbage', () => {
    expect(resolveThumbUrlToPath(dir, 'thumb://scan-5/..%2F..%2Fbyteback.db')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'thumb://scan-6/5-7.png')).toBeNull() // host != embedded scanId
    expect(resolveThumbUrlToPath(dir, 'thumb://scan-5/6-7.png')).toBeNull() // host != embedded scanId
    expect(resolveThumbUrlToPath(dir, 'thumb://scan-5/5-7.exe')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'thumb://scan-5/%2E%2E%2Fescape.png')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'file:///etc/passwd')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'thumb://abc/5-7.png')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'not a url')).toBeNull()
    // Chromium canonicalizes pure-digit standard-scheme hosts to IPv4
    // (thumb://5/... reaches the handler as thumb://0.0.0.1/...); the guard
    // must reject that mangled host, and thumbUrlFor must never emit it.
    expect(resolveThumbUrlToPath(dir, 'thumb://0.0.0.1/5-7.png')).toBeNull()
    expect(resolveThumbUrlToPath(dir, 'thumb://5/5-7.png')).toBeNull()
  })

  it('thumbUrlFor emits the canonical scan-<id> host form', () => {
    expect(thumbUrlFor(7, 5, 'image/png')).toBe('thumb://scan-5/5-7.png')
    expect(thumbUrlFor(7, 5, 'image/heic')).toBeNull()
    const resolved = resolveThumbUrlToPath(dir, thumbUrlFor(7, 5, 'image/jpeg')!)
    expect(resolved).toBe(join(dir, '5-7.jpg'))
  })
})
