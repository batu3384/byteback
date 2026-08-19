export function sourceDisplayLabel(source?: string): string {
  if (source === 'apfs_container' || source === 'apfs_volume') {
    return 'APFS volume keşfi (katalog yok)'
  }
  if (source === 'hfs_limit') return 'HFS katalog tavanı'
  return source ?? ''
}
