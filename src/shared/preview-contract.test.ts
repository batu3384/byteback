import { describe, it, expect } from 'vitest'
import type { FilePreviewResult } from './ipc-contract'
import { formatPreviewHex, isImagePreviewKind, previewDataUrl, sniffImageMime } from './preview-utils'

describe('preview contract', () => {
  it('formats hex dump with offset column', () => {
    const hex = formatPreviewHex(new Uint8Array([0x48, 0x45, 0x4c, 0x4c, 0x4f]), 16)
    expect(hex).toContain('0000')
    expect(hex).toContain('48 45 4c 4c 4f')
    expect(hex).toContain('HELLO')
  })

  it('uses real image MIME from magic bytes', () => {
    const jpeg = new Uint8Array([0xff, 0xd8, 0xff, 0xdb, 0x00])
    expect(sniffImageMime(jpeg)).toBe('image/jpeg')
    const mock: FilePreviewResult = {
      success: true,
      kind: 'image',
      mime: 'image/jpeg',
      data: jpeg,
    }
    expect(isImagePreviewKind(mock.kind)).toBe(true)
    expect(previewDataUrl(mock)?.startsWith('data:image/jpeg;base64,')).toBe(true)
  })

  it('sniffs png when mime omitted', () => {
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00])
    const mock: FilePreviewResult = { success: true, kind: 'image', data: png }
    expect(previewDataUrl(mock)?.startsWith('data:image/png;base64,')).toBe(true)
  })

  it('returns null data url when image magic missing', () => {
    const mock: FilePreviewResult = {
      success: true,
      kind: 'image',
      data: new Uint8Array([0x00, 0x01, 0x02, 0x03]),
    }
    expect(previewDataUrl(mock)).toBeNull()
  })

  it('returns null data url for failed preview', () => {
    const mock: FilePreviewResult = { success: false, error: 'disk closed' }
    expect(previewDataUrl(mock)).toBeNull()
  })

  it('passes through structural note from native', () => {
    const mock: FilePreviewResult = {
      success: true,
      kind: 'binary',
      note: 'H.264 · IDR kare @ +47 · decode yok',
      data: new Uint8Array([0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70]),
    }
    expect(mock.note).toContain('IDR')
  })
})
