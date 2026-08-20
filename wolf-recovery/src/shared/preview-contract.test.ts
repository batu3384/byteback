import { describe, it, expect } from 'vitest'
import type { FilePreviewResult } from './ipc-contract'
import { formatPreviewHex, isImagePreviewKind, previewDataUrl } from './preview-utils'

describe('preview contract', () => {
  it('formats hex dump with offset column', () => {
    const hex = formatPreviewHex(new Uint8Array([0x48, 0x45, 0x4c, 0x4c, 0x4f]), 16)
    expect(hex).toContain('0000')
    expect(hex).toContain('48 45 4c 4c 4f')
    expect(hex).toContain('HELLO')
  })

  it('recognizes image preview kinds', () => {
    const mock: FilePreviewResult = {
      success: true,
      kind: 'image',
      data: new Uint8Array([0xff, 0xd8, 0xff, 0xdb]),
    }
    expect(isImagePreviewKind(mock.kind)).toBe(true)
    expect(previewDataUrl(mock)?.startsWith('data:image/*;base64,')).toBe(true)
  })

  it('returns null data url for failed preview', () => {
    const mock: FilePreviewResult = { success: false, error: 'disk closed' }
    expect(previewDataUrl(mock)).toBeNull()
  })
})
