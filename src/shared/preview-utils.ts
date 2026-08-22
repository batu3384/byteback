import type { FilePreviewResult } from './ipc-contract'

export function formatPreviewHex(data: Uint8Array, max = 256): string {
  const lines: string[] = []
  const n = Math.min(data.length, max)
  for (let i = 0; i < n; i += 16) {
    const slice = data.subarray(i, Math.min(i + 16, n))
    const hex = Array.from(slice, (b) => b.toString(16).padStart(2, '0')).join(' ')
    const ascii = Array.from(slice, (b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : '.')).join('')
    lines.push(`${i.toString(16).padStart(4, '0')}  ${hex.padEnd(16 * 3 - 1, ' ')}  ${ascii}`)
  }
  return lines.join('\n')
}

export function isImagePreviewKind(kind?: FilePreviewResult['kind']): boolean {
  return kind === 'image'
}

/** Magic-byte sniff for common image MIME types. Returns null if unknown. */
export function sniffImageMime(data: Uint8Array): string | null {
  if (data.length >= 3 && data[0] === 0xff && data[1] === 0xd8 && data[2] === 0xff) return 'image/jpeg'
  if (
    data.length >= 8 &&
    data[0] === 0x89 &&
    data[1] === 0x50 &&
    data[2] === 0x4e &&
    data[3] === 0x47 &&
    data[4] === 0x0d &&
    data[5] === 0x0a &&
    data[6] === 0x1a &&
    data[7] === 0x0a
  ) {
    return 'image/png'
  }
  if (
    data.length >= 6 &&
    data[0] === 0x47 &&
    data[1] === 0x49 &&
    data[2] === 0x46 &&
    data[3] === 0x38 &&
    (data[4] === 0x37 || data[4] === 0x39) &&
    data[5] === 0x61
  ) {
    return 'image/gif'
  }
  if (
    data.length >= 12 &&
    data[0] === 0x52 &&
    data[1] === 0x49 &&
    data[2] === 0x46 &&
    data[3] === 0x46 &&
    data[8] === 0x57 &&
    data[9] === 0x45 &&
    data[10] === 0x42 &&
    data[11] === 0x50
  ) {
    return 'image/webp'
  }
  if (data.length >= 14 && data[0] === 0x42 && data[1] === 0x4d) {
    const bfOff =
      data[10]! | (data[11]! << 8) | (data[12]! << 16) | (data[13]! << 24)
    if (bfOff >= 14 && bfOff <= 1024 * 1024) {
      if (data.length >= 18) {
        const dib = data[14]! | (data[15]! << 8) | (data[16]! << 16) | (data[17]! << 24)
        if (dib === 12 || dib === 40 || dib === 108 || dib === 124) return 'image/bmp'
      }
    }
  }
  return null
}

export function resolvePreviewImageMime(preview: FilePreviewResult): string | null {
  if (preview.mime && preview.mime.startsWith('image/')) return preview.mime
  if (!preview.data?.length) return null
  return sniffImageMime(preview.data)
}

export function previewDataUrl(preview: FilePreviewResult): string | null {
  if (!preview.success || !preview.data?.length || !isImagePreviewKind(preview.kind)) return null
  const mime = resolvePreviewImageMime(preview)
  if (!mime) return null
  const bytes = preview.data
  let binary = ''
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]!)
  return `data:${mime};base64,${btoa(binary)}`
}
