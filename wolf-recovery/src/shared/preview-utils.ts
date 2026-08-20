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

export function previewDataUrl(preview: FilePreviewResult): string | null {
  if (!preview.success || !preview.data?.length || !isImagePreviewKind(preview.kind)) return null
  const bytes = preview.data
  let binary = ''
  for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]!)
  return `data:image/*;base64,${btoa(binary)}`
}
