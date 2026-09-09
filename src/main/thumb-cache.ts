// FAZ 1.3b — main-side gallery thumbnail disk cache (L2 behind the renderer's
// in-memory L1). Pure fs/path helpers live here (no electron import) so the
// name mapping, LRU cap math and the thumb:// traversal guard are unit
// testable; main.ts/ipc-handlers.ts wire them to app.getPath('userData').
import { existsSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync } from 'fs'
import { join, resolve, sep } from 'path'

/** LRU cap for the thumbs directory (spec: 256 MB). */
export const THUMB_CACHE_CAP_BYTES = 256 * 1024 * 1024

/** Cache directory name under userData. */
export const THUMB_DIR_NAME = 'thumbs'

/** Decoded-payload cap for put-thumb IPC writes (previews are <=64KB payloads). */
export const THUMB_MAX_BYTES = 512 * 1024

const EXT_BY_MIME: Record<string, string> = {
  'image/jpeg': '.jpg',
  'image/png': '.png',
  'image/gif': '.gif',
  'image/webp': '.webp',
  'image/bmp': '.bmp',
}

export function extForMime(mime: string): string | null {
  return Object.prototype.hasOwnProperty.call(EXT_BY_MIME, mime) ? EXT_BY_MIME[mime]! : null
}

export function mimeForExt(ext: string): string | null {
  const e = ext.toLowerCase()
  for (const [mime, candidate] of Object.entries(EXT_BY_MIME)) {
    if (candidate === `.${e}`) return mime
  }
  return null
}

/** Cached file name `<scanId>-<fileId>.<ext>`; null for unsupported mime or
 *  non-positive ids (the ids become the file name — they must be plain digits). */
export function thumbFileName(fileId: number, scanId: number, mime: string): string | null {
  if (!Number.isInteger(fileId) || fileId <= 0 || !Number.isInteger(scanId) || scanId <= 0) return null
  const ext = extForMime(mime)
  return ext ? `${scanId}-${fileId}${ext}` : null
}

/** Absolute cache path for a thumbnail, or null when the mime/id are invalid. */
export function thumbPathFor(dir: string, fileId: number, scanId: number, mime: string): string | null {
  const name = thumbFileName(fileId, scanId, mime)
  return name ? join(dir, name) : null
}

/** Canonical thumb:// URL for a cached thumbnail (form understood by
 *  resolveThumbUrlToPath); null for unsupported mime/invalid ids. */
export function thumbUrlFor(fileId: number, scanId: number, mime: string): string | null {
  const name = thumbFileName(fileId, scanId, mime)
  return name ? `thumb://scan-${scanId}/${encodeURIComponent(name)}` : null
}

/** Accepted cache file names — digits only, so URL/path traversal cannot smuggle. */
const NAME_RE = /^(\d+)-(\d+)\.(jpg|png|gif|webp|bmp)$/

export interface ThumbDirEntry {
  name: string
  sizeBytes: number
  mtimeMs: number
}

export function listThumbEntries(dir: string): ThumbDirEntry[] {
  try {
    return readdirSync(dir)
      .filter((n) => NAME_RE.test(n))
      .map((n) => {
        const st = statSync(join(dir, n))
        return { name: n, sizeBytes: st.size, mtimeMs: st.mtimeMs }
      })
  } catch {
    return []
  }
}

/** Pure LRU eviction plan: oldest-by-mtime names to delete until the total is
 *  under cap. The just-written file (keepName) is never its own victim — if a
 *  single thumb exceeds the cap, everything else goes and the oversized file
 *  stays until the next write evicts it. */
export function planEvictions(entries: ThumbDirEntry[], capBytes: number, keepName?: string): string[] {
  const total = entries.reduce((s, e) => s + e.sizeBytes, 0)
  if (total <= capBytes) return []
  const victims = entries
    .filter((e) => e.name !== keepName)
    .sort((a, b) => a.mtimeMs - b.mtimeMs)
  let running = total
  const out: string[] = []
  for (const e of victims) {
    if (running <= capBytes) break
    out.push(e.name)
    running -= e.sizeBytes
  }
  return out
}

/** Write one thumbnail and enforce the LRU cap afterwards. Returns the written
 *  path, or null on invalid input/unsupported mime. IO failures throw — the
 *  IPC layer turns them into a null reply (thumbnail caching is best-effort). */
export function storeThumb(dir: string, fileId: number, scanId: number, mime: string, data: Buffer): string | null {
  const name = thumbFileName(fileId, scanId, mime)
  if (!name || !Buffer.isBuffer(data) || data.length === 0 || data.length > THUMB_MAX_BYTES) return null
  mkdirSync(dir, { recursive: true })
  const path = join(dir, name)
  writeFileSync(path, data)
  for (const victim of planEvictions(listThumbEntries(dir), THUMB_CACHE_CAP_BYTES, name)) {
    try {
      rmSync(join(dir, victim))
    } catch {
      /* best-effort LRU */
    }
  }
  return path
}

/** Locate an existing cache entry across the known mime extensions. */
export function findThumbPath(dir: string, fileId: number, scanId: number): string | null {
  if (!Number.isInteger(fileId) || fileId <= 0 || !Number.isInteger(scanId) || scanId <= 0) return null
  for (const ext of Object.values(EXT_BY_MIME)) {
    const p = join(dir, `${scanId}-${fileId}${ext}`)
    if (existsSync(p)) return p
  }
  return null
}

/**
 * Strict thumb:// URL resolution — the protocol handler's traversal guard.
 * Accepts ONLY `thumb://scan-<scanId>/<scanId>-<fileId>.<ext>`. The host must
 * carry the `scan-` prefix because Chromium canonicalizes standard-scheme
 * hosts that are pure digits into IPv4 addresses (`thumb://1/...` would reach
 * the handler as `thumb://0.0.0.1/...`); the host must equal the scanId
 * embedded in the file name, and the resolved absolute path must stay
 * strictly inside `dir`. Anything else -> null (handler 403s).
 */
export function resolveThumbUrlToPath(dir: string, url: string): string | null {
  let parsed: URL
  try {
    parsed = new URL(url)
  } catch {
    return null
  }
  if (parsed.protocol !== 'thumb:') return null
  const host = parsed.hostname
  let name: string
  try {
    name = decodeURIComponent(parsed.pathname.replace(/^\/+/, ''))
  } catch {
    return null
  }
  const hostScan = /^scan-(\d+)$/.exec(host)
  if (!hostScan || !NAME_RE.test(name)) return null
  const embedded = /^(\d+)-(\d+)\./.exec(name)
  if (!embedded || embedded[1] !== hostScan[1]) return null
  const root = resolve(dir)
  const full = resolve(root, name)
  if (full === root || !full.startsWith(root + sep)) return null
  return full
}
