export function sourceDisplayLabel(source?: string): string {
  if (source === 'apfs_container' || source === 'apfs_volume') {
    return 'APFS volume keşfi (katalog yok)'
  }
  if (source === 'hfs_limit') return 'HFS katalog tavanı'
  if (source === 'vss_unbound') return 'VSS bağlanmadı (host karışımı kapalı)'
  if (source === 'bitlocker_detect') return 'BitLocker (şifre çözme yok)'
  return source ?? ''
}
