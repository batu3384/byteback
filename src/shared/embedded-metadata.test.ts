import { describe, expect, it } from 'vitest'
import { extractJpegExifUnix, extractPdfInfo, sniffMediaContainer } from './embedded-metadata'

function minimalJpegWithExif(dateStr: string): Uint8Array {
  const dateBytes = new TextEncoder().encode(dateStr)
  const ifdOffset = 8
  const valueOffset = ifdOffset + 2 + 12 + 4 // after IFD header + 1 entry + next-IFD ptr
  const tiffSize = valueOffset + dateBytes.length
  const exifPayload = 6 + tiffSize
  const segLen = exifPayload + 2

  const buf: number[] = [0xff, 0xd8, 0xff, 0xe1]
  buf.push((segLen >> 8) & 0xff, segLen & 0xff)
  buf.push(...[0x45, 0x78, 0x69, 0x66, 0x00, 0x00])
  buf.push(0x49, 0x49, 0x2a, 0x00, ifdOffset & 0xff, (ifdOffset >> 8) & 0xff, 0x00, 0x00)
  buf.push(0x01, 0x00)
  buf.push(0x03, 0x90, 0x02, 0x00)
  buf.push(dateBytes.length & 0xff, (dateBytes.length >> 8) & 0xff, 0x00, 0x00)
  buf.push(valueOffset & 0xff, (valueOffset >> 8) & 0xff, 0x00, 0x00)
  buf.push(0x00, 0x00, 0x00, 0x00)
  for (const b of dateBytes) buf.push(b)
  buf.push(0xff, 0xd9)
  return new Uint8Array(buf)
}

describe('embedded-metadata', () => {
  it('extracts JPEG EXIF DateTimeOriginal as unix seconds', () => {
    const jpeg = minimalJpegWithExif('2024:06:15 12:30:45\0')
    const unix = extractJpegExifUnix(jpeg)
    expect(unix).not.toBeNull()
    expect(unix).toBe(Math.floor(Date.UTC(2024, 5, 15, 12, 30, 45) / 1000))
  })

  it('returns null for non-JPEG', () => {
    expect(extractJpegExifUnix(new Uint8Array([0x89, 0x50, 0x4e, 0x47]))).toBeNull()
  })

  it('parses PDF header metadata from prefix', () => {
    const pdf = new TextEncoder().encode(
      '%PDF-1.7\n1 0 obj\n<< /Title (Test Doc) /CreationDate (D:20240615123045) >>\n',
    )
    const info = extractPdfInfo(pdf)
    expect(info.version).toBe('1.7')
    expect(info.title).toBe('Test Doc')
    expect(info.creationDate).toBe('2024-06-15 12:30')
  })

  it('sniffs common media containers', () => {
    const mp4 = new Uint8Array(12)
    mp4.set([0, 0, 0, 0x18, 0x66, 0x74, 0x79, 0x70, 0x69, 0x73, 0x6f, 0x6d])
    expect(sniffMediaContainer(mp4)?.label).toContain('MP4')
    const id3 = new Uint8Array(12)
    id3.set([0x49, 0x44, 0x33, 0x04, 0, 0, 0, 0, 0, 0, 0, 0])
    expect(sniffMediaContainer(id3)?.kind).toBe('audio')
  })
})
