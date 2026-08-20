export function sourceDisplayLabel(source?: string): string {
  if (source === 'apfs_container' || source === 'apfs_volume') {
    return 'APFS volume (omap/fs_oid + APSB)'
  }
  if (source === 'apfs_file') return 'APFS katalog'
  if (source === 'hfs_limit') return 'HFS katalog tavanı'
  if (source === 'vss_unbound') return 'VSS bağlanmadı (host karışımı kapalı)'
  if (source === 'vss_bind') return 'VSS (volume serial bağlandı)'
  if (source === 'bitlocker_detect') return 'BitLocker (FVE yok / anahtar yok)'
  if (source === 'bitlocker_fve') return 'BitLocker FVE metadata'
  return source ?? ''
}
