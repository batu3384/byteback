/** Honest embedded timestamps from file headers (not FS metadata). */

/** Parse JPEG EXIF DateTimeOriginal / DateTime → unix seconds, or null. */
export function extractJpegExifUnix(data: Uint8Array): number | null {
  if (data.length < 22 || data[0] !== 0xff || data[1] !== 0xd8) return null
  let i = 2
  while (i + 4 < data.length) {
    if (data[i] !== 0xff) {
      i++
      continue
    }
    const marker = data[i + 1]!
    if (marker === 0xda || marker === 0xd9) break
    const segLen = (data[i + 2]! << 8) | data[i + 3]!
    if (segLen < 2 || i + 2 + segLen > data.length) break
    if (marker === 0xe1 && segLen >= 8) {
      const exif = data.subarray(i + 4, i + 2 + segLen)
      if (
        exif.length >= 6 &&
        exif[0] === 0x45 &&
        exif[1] === 0x78 &&
        exif[2] === 0x69 &&
        exif[3] === 0x66 &&
        exif[4] === 0 &&
        exif[5] === 0
      ) {
        const t = parseExifTiff(exif.subarray(6))
        if (t != null) return t
      }
    }
    i += 2 + segLen
  }
  return null
}

function parseExifTiff(tiff: Uint8Array): number | null {
  if (tiff.length < 8) return null
  const le = tiff[0] === 0x49 && tiff[1] === 0x49
  const be = tiff[0] === 0x4d && tiff[1] === 0x4d
  if (!le && !be) return null
  const rd16 = (p: number) => (le ? tiff[p]! | (tiff[p + 1]! << 8) : (tiff[p]! << 8) | tiff[p + 1]!)
  const rd32 = (p: number) =>
    le
      ? tiff[p]! | (tiff[p + 1]! << 8) | (tiff[p + 2]! << 16) | (tiff[p + 3]! << 24)
      : (tiff[p]! << 24) | (tiff[p + 1]! << 16) | (tiff[p + 2]! << 8) | tiff[p + 3]!
  const ifd0 = rd32(4)
  return ifdTagUnix(tiff, ifd0, le, 0x9003) ?? ifdTagUnix(tiff, ifd0, le, 0x0132)
}

function ifdTagUnix(tiff: Uint8Array, ifdOff: number, le: boolean, tagWanted: number): number | null {
  if (ifdOff + 2 > tiff.length) return null
  const rd16 = (p: number) => (le ? tiff[p]! | (tiff[p + 1]! << 8) : (tiff[p]! << 8) | tiff[p + 1]!)
  const rd32 = (p: number) =>
    le
      ? tiff[p]! | (tiff[p + 1]! << 8) | (tiff[p + 2]! << 16) | (tiff[p + 3]! << 24)
      : (tiff[p]! << 24) | (tiff[p + 1]! << 16) | (tiff[p + 2]! << 8) | tiff[p + 3]!
  const count = rd16(ifdOff)
  let pos = ifdOff + 2
  for (let i = 0; i < count && pos + 12 <= tiff.length; i++, pos += 12) {
    const tag = rd16(pos)
    const type = rd16(pos + 2)
    const cnt = rd32(pos + 4)
    if (tag !== tagWanted || type !== 2 || cnt < 19) continue
    const valOff = rd32(pos + 8)
    let str: string
    if (cnt > 4) {
      if (valOff + cnt > tiff.length) continue
      str = new TextDecoder('ascii').decode(tiff.subarray(valOff, valOff + cnt))
    } else {
      str = new TextDecoder('ascii').decode(tiff.subarray(pos + 8, pos + 8 + cnt))
    }
    const unix = parseExifDateString(str)
    if (unix != null) return unix
  }
  return null
}

function parseExifDateString(s: string): number | null {
  const m = /^(\d{4}):(\d{2}):(\d{2}) (\d{2}):(\d{2}):(\d{2})/.exec(s)
  if (!m) return null
  const y = Number(m[1])
  const mo = Number(m[2])
  const d = Number(m[3])
  const h = Number(m[4])
  const mi = Number(m[5])
  const se = Number(m[6])
  if (y < 1970 || y > 2100) return null
  const ms = Date.UTC(y, mo - 1, d, h, mi, se)
  return ms > 0 ? Math.floor(ms / 1000) : null
}

export type PdfPreviewInfo = {
  version?: string
  creationDate?: string
  title?: string
}

/** Best-effort PDF header metadata from prefix bytes. */
export function extractPdfInfo(data: Uint8Array): PdfPreviewInfo {
  const text = new TextDecoder('latin1').decode(data.subarray(0, Math.min(data.length, 8192)))
  const info: PdfPreviewInfo = {}
  const ver = /^%PDF-(\d\.\d)/m.exec(text)
  if (ver) info.version = ver[1]
  const cre = /\/CreationDate\s*\((D:[^)]+)\)/.exec(text) || /\/CreationDate\s*\(([^)]+)\)/.exec(text)
  if (cre) info.creationDate = formatPdfDate(cre[1]!)
  const title = /\/Title\s*\(([^)]+)\)/.exec(text)
  if (title) info.title = title[1]!.replace(/\\(\d{3})/g, (_, o) => String.fromCharCode(Number(o)))
  return info
}

function formatPdfDate(raw: string): string {
  const d = raw.startsWith('D:') ? raw.slice(2) : raw
  if (d.length < 8) return raw
  const y = d.slice(0, 4)
  const mo = d.slice(4, 6)
  const day = d.slice(6, 8)
  const hh = d.length >= 10 ? d.slice(8, 10) : '00'
  const mm = d.length >= 12 ? d.slice(10, 12) : '00'
  return `${y}-${mo}-${day} ${hh}:${mm}`
}

export type MediaContainerHint = {
  kind: 'video' | 'audio'
  label: string
}

/** Magic sniff for common video/audio containers in prefix bytes. */
export function sniffMediaContainer(data: Uint8Array): MediaContainerHint | null {
  if (data.length < 12) return null
  if (data[0] === 0x1a && data[1] === 0x45 && data[2] === 0xdf && data[3] === 0xa3) {
    return { kind: 'video', label: 'Matroska/WebM (EBML)' }
  }
  if (data.length >= 8 && data[4] === 0x66 && data[5] === 0x74 && data[6] === 0x79 && data[7] === 0x70) {
    const brand = new TextDecoder('ascii').decode(data.subarray(8, 12)).replace(/\0/g, '')
    return { kind: 'video', label: brand ? `MP4 family (${brand})` : 'MP4 family' }
  }
  if (data[0] === 0x52 && data[1] === 0x49 && data[2] === 0x46 && data[3] === 0x46) {
    if (data[8] === 0x41 && data[9] === 0x56 && data[10] === 0x49) return { kind: 'video', label: 'AVI (RIFF)' }
    if (data[8] === 0x57 && data[9] === 0x41 && data[10] === 0x56) return { kind: 'audio', label: 'WAV (RIFF)' }
    if (data[8] === 0x57 && data[9] === 0x45 && data[10] === 0x42) return { kind: 'video', label: 'WebM (RIFF)' }
  }
  if (data[0] === 0x49 && data[1] === 0x44 && data[2] === 0x33) return { kind: 'audio', label: 'MP3 (ID3)' }
  if (data[0] === 0xff && (data[1]! & 0xe0) === 0xe0) return { kind: 'audio', label: 'MP3 frame' }
  if (data[0] === 0x4f && data[1] === 0x67 && data[2] === 0x67 && data[3] === 0x53) return { kind: 'audio', label: 'Ogg' }
  if (data[0] === 0x66 && data[1] === 0x4c && data[2] === 0x61 && data[3] === 0x43) return { kind: 'audio', label: 'FLAC' }
  return null
}
