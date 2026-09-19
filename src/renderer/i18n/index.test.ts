import { describe, expect, it, beforeAll } from 'vitest'
import { localizeNote, setLang, getLang, t, tFormat, localeTag, formatInt } from './index'

// CA-052: native machine codes must localize; unknown text passes through.
describe('localizeNote', () => {
  beforeAll(() => setLang('en'))

  it('localizes plain codes', () => {
    expect(localizeNote('video.ffmpeg.missing')).toBe('Video · FFmpeg not found (PATH or BYTEBACK_FFMPEG)')
    expect(localizeNote('video.ffmpeg.first_frame')).toBe('FFmpeg first frame')
    expect(localizeNote('bitlocker.tpm_only')).toBe('TPM protector only — no password/recovery path')
  })

  it('keeps the numeric detail tail after the code', () => {
    const out = localizeNote('video.h264.idr_frame:@ +8192 640x480')
    expect(out).toBe('Video · H.264 IDR frame @ +8192 640x480 · no decoder')
  })

  it('passes unknown or legacy text through unchanged', () => {
    expect(localizeNote('totally unknown text')).toBe('totally unknown text')
    expect(localizeNote('')).toBe('')
    expect(localizeNote(undefined)).toBe('')
  })

  it('switches languages', () => {
    setLang('tr')
    expect(localizeNote('bitlocker.tpm_only')).toBe('Yalnız TPM koruyucusu — parola/kurtarma yolu yok')
    expect(t('nav.results')).toBe('Sonuçlar')
    setLang('en')
    expect(t('nav.results')).toBe('Results')
    expect(getLang()).toBe('en')
  })

  it('tFormat interpolates tokens', () => {
    setLang('en')
    expect(tFormat('report.chainOk', { n: '12' })).toBe('Verified — 12 entries intact (SHA-256 chain)')
  })

  it('tFormat replaces repeated tokens (N2 regression)', () => {
    setLang('en')
    const out = tFormat('results.confirmDestOnDrive', { a: 'x' })
    // The sentence mentions the drive warning more than once; every {a} must go.
    expect(out).not.toContain('{a}')
  })
})

// Lane-5 sweep: new error-surface keys must exist in both locales and resolve.
describe('lane-5 sweep keys', () => {
  beforeAll(() => setLang('tr'))

  const keys = [
    'common.close',
    'smart.na',
    'tl.loadFailed',
    'case.loadFailed',
    'shred.drivesFailed',
    'raid.drivesFailed',
    'raid.failMemberFailed',
    'raid.slot',
    'raid.raid0',
    'raid.raid1',
    'raid.raid5',
    'raid.raid10',
    'raid.raid1e',
    'raid.jbod',
    'raid.stripeLabel',
    'raid.offsetLabel',
    'raid.stripeAmbiguous',
    'report.generateFailed',
    'kw.contentBindFailed',
    'kw.contentUnread',
    'kw.contentQueryTooLong',
    'kw.regexRejected',
    'kw.regexUnsafe',
  ] as const

  it.each(keys)('resolves %s in tr and en', (key) => {
    setLang('tr')
    const tr = t(key)
    expect(tr).not.toBe(key)
    setLang('en')
    const en = t(key)
    expect(en).not.toBe(key)
  })

  it('formats the raid slot label', () => {
    setLang('tr')
    expect(tFormat('raid.slot', { n: '2' })).toBe('Yuva 2')
    setLang('en')
    expect(tFormat('raid.slot', { n: '2' })).toBe('Slot 2')
  })

  it('formats the report generation failure', () => {
    setLang('tr')
    expect(tFormat('report.generateFailed', { err: 'x' })).toBe('Rapor oluşturulamadı: x')
    setLang('en')
    expect(tFormat('report.generateFailed', { err: 'x' })).toBe('Could not generate the report: x')
  })

  it('report.allocatedTd tr slot is Turkish, not the EN copy', () => {
    setLang('tr')
    expect(t('report.allocatedTd')).toContain('Tahsisli')
    setLang('en')
    expect(t('report.allocatedTd')).toContain('Allocated')
  })
})

// Lane-C sweep: engine phase labels + disk map + results load-error keys.
describe('lane-C sweep keys', () => {
  const keys = [
    'scan.phase.metadata',
    'scan.phase.carve',
    'scan.phase.carveOnly',
    'scan.phase.carveSkipped',
    'scan.phase.carveFallback',
    'scan.phase.carveBitmapUnread',
    'scan.carveSkipped',
    'diskmap.title',
    'diskmap.hint',
    'diskmap.records',
    'diskmap.deleted',
    'results.loadErrorTitle',
    'results.loadErrorBody',
    'chrome.skipToMain',
    'chrome.statusIdle',
    'dash.volumeSpanned',
    'dash.volumeLettersFailed',
    'dash.imageScanTitle',
    'dash.imageScanHint',
    'dash.imageScanPick',
    'dash.imageScanNeedFile',
    'dash.imageScanNone',
    'dash.lastCrashDump',
    'hex.sourceLabel',
    'hex.volumeDevice',
    'hex.raidArray',
    'hex.ascii',
    'hex.search',
    'hex.searchPlaceholder',
    'hex.noHits',
    'hex.searchUnread',
    'hex.searchInvalid',
    'hex.searching',
    'hex.mftGo',
    'hex.mftUnread',
    'hex.markAdd',
    'hex.marks',
    'imager.useVolume',
    'imager.spannedNote',
    'imager.raidNote',
    'imager.drivesFailed',
    'imager.raidStateFailed',
    'imager.noDriveTitle',
    'imager.noDriveBody',
    'hex.raidStateFailed',
    'results.raidStateFailed',
    'results.nsrlUnread',
    'report.caseUnread',
    'case.loadFailedTitle',
    'case.nsrlStatsFailed',
    'dash.sessionLoadFailed',
    'dash.sessionLogUnread',
    'dash.engineFailed',
    'drive.trimUnread',
    'drive.partTableUnread',
    'dash.lostUnread',
    'dash.lostUnreadShort',
    'dash.lostUnreadEmpty',
    'ssd.titleUnread',
    'ssd.bodyUnread',
    'results.previewWhileBusy',
    'results.analyzeDedup',
    'results.analyzingDedup',
    'results.analyzeDedupDone',
    'results.analyzeDedupBusy',
    'results.analyzeDedupFailed',
    'results.sameContent',
    'results.sameContentTitle',
    'results.showMft',
    'source.hfs_catalog',
    'source.hfs_catalog_unused',
    'source.hfs_journal',
    'source.hfs_vol_name',
    'source.xfs_unlinked',
    'source.hfs_catalog_unread',
    'source.hfs_linear_unread',
    'source.apfs_nxsb_unread',
    'source.apfs_block_unread',
    'source.apfs_linear_unread',
    'source.apfs_catalog_unread',
    'source.apfs_omap_deleted',
    'source.refs_supb_unread',
    'source.refs_page_unread',
    'source.refs_volume',
    'source.fat_dir_unread',
    'source.fat_chain_unread',
    'source.fat_vol_label',
    'source.iso9660_vol_id',
    'source.exfat_vol_label',
    'source.udf_vol_id',
    'source.udf_pvd_id',
    'source.udf_fsd_id',
    'source.ext4_dir_unread',
    'source.ext4_journal',
    'source.ext4_journal_unread',
    'source.ext4_vol_name',
    'source.ext4_journal_replay',
    'source.xfs_dir_unread',
    'source.xfs_sb_unread',
    'source.xfs_inode_unread',
    'source.xfs_bmap_unread',
    'source.xfs_vol_name',
    'source.ntfs_i30_unread',
    'source.ntfs_i30_unalloc',
    'source.ntfs_i30_carve',
    'source.unalloc_map_unread',
    'source.ntfs_logfile_unread',
    'source.usn_unread',
    'source.ntfs_mft_unread',
    'source.ntfs_efs',
    'source.ntfs_object_id',
    'source.ntfs_vol_name',
    'source.probe_unread',
    'source.carver_unread',
    'scan.refsProbeCapped',
    'scan.fatDirUnread',
    'scan.ext4DirUnread',
    'scan.xfsDirUnread',
    'scan.ntfsI30Unread',
    'scan.ntfsI30Unalloc',
    'scan.unallocMapUnread',
    'scan.ntfsLogfileUnread',
    'scan.usnUnread',
    'scan.ntfsMftUnread',
    'scan.probeUnread',
    'scan.carverUnread',
    'scan.hfsCatalogUnread',
    'scan.apfsNxsbUnread',
    'scan.refsSupbUnread',
    'scan.listFailed',
    'scan.honestyLoadFailed',
    'results.refsProbeCapped',
    'results.fatDirUnread',
    'results.ext4DirUnread',
    'results.xfsDirUnread',
    'results.ntfsI30Unread',
    'results.ntfsI30Unalloc',
    'results.unallocMapUnread',
    'results.ntfsLogfileUnread',
    'results.usnUnread',
    'results.ntfsMftUnread',
    'results.probeUnread',
    'results.carverUnread',
    'results.hfsCatalogUnread',
    'results.apfsNxsbUnread',
    'results.refsSupbUnread',
    'results.honestyLoadFailed',
    'report.chainReadFailed',
    'report.emptyBody',
    'report.auditUnread',
    'case.overlayTitle',
  ] as const

  it.each(keys)('resolves %s in tr and en', (key) => {
    setLang('tr')
    expect(t(key)).not.toBe(key)
    setLang('en')
    expect(t(key)).not.toBe(key)
  })

  it('carveSkipped copy no longer blames unsupported file systems (CA-022)', () => {
    setLang('tr')
    expect(t('scan.carveSkipped')).not.toContain('APFS')
    setLang('en')
    expect(t('scan.carveSkipped')).not.toContain('APFS')
  })

  it('formats the disk map record counts', () => {
    setLang('tr')
    expect(tFormat('diskmap.records', { n: '5' })).toBe('5 kayıt')
    expect(tFormat('diskmap.deleted', { n: '2' })).toBe('silinmiş: 2')
    setLang('en')
    expect(tFormat('diskmap.records', { n: '5' })).toBe('5 records')
  })
})

// Renderer sweep: App-level scan status keys, percent convention, chrome.
describe('renderer sweep keys', () => {
  const keys = [
    'scan.waiting',
    'scan.running',
    'scan.resuming',
    'scan.raidRunning',
    'scan.stoppingStatus',
    'scan.pausedResumable',
    'scan.failedDb',
    'scan.apiMissing',
    'scan.startFailed',
    'scan.failedWith',
    'scan.raidApiMissing',
    'scan.raidStartFailed',
    'scan.raidFailedWith',
    'common.percent',
    'sidebar.version',
    'dash.lastScanTitle',
    'dash.sessionLogUnread',
    'dash.logPrefix',
    'dash.clearConfirmTitle',
    'dash.clearConfirmYes',
  ] as const

  it.each(keys)('resolves %s in tr and en', (key) => {
    setLang('tr')
    expect(t(key)).not.toBe(key)
    setLang('en')
    expect(t(key)).not.toBe(key)
  })

  it('percent follows each locale convention (TR prefix, EN suffix)', () => {
    setLang('tr')
    expect(tFormat('common.percent', { n: '42' })).toBe('%42')
    setLang('en')
    expect(tFormat('common.percent', { n: '42' })).toBe('42%')
  })

  it('interpolates engine error detail into scan failure statuses', () => {
    setLang('en')
    expect(tFormat('scan.failedDb', { err: 'disk I/O' })).toBe('Database unavailable: disk I/O')
    expect(tFormat('scan.failedWith', { err: 'EACCES' })).toBe('Scan error: EACCES')
    setLang('tr')
    expect(tFormat('scan.failedDb', { err: 'disk I/O' })).toBe('Veritabanı kullanılamıyor: disk I/O')
  })

  it('localeTag/formatInt follow the active language', () => {
    setLang('en')
    expect(localeTag()).toBe('en-US')
    expect(formatInt(12345)).toBe('12,345')
    setLang('tr')
    expect(localeTag()).toBe('tr-TR')
    expect(formatInt(12345)).toBe('12.345')
  })
})

// Renderer/UI fix lane: new keys + terminology unification (results.allTypes
// removed in favor of results.all). Also proves setLang is safe in a non-DOM
// environment (document.documentElement.lang sync is guarded).
describe('renderer ui-lane keys', () => {
  const keys = [
    'smart.emptyTitle',
    'results.col.path',
  ] as const

  it.each(keys)('resolves %s in tr and en', (key) => {
    setLang('tr')
    expect(t(key)).not.toBe(key)
    setLang('en')
    expect(t(key)).not.toBe(key)
  })

  it('unifies the "all" filter term on Tümü (results.allTypes is gone)', () => {
    setLang('tr')
    expect(t('results.all')).toBe('Tümü')
    expect(t('results.allTypes')).toBe('results.allTypes') // key deleted -> falls back
  })

  it('smart.emptyTitle no longer borrows the hex empty-state wording', () => {
    setLang('en')
    expect(t('smart.emptyTitle')).toBe('No Drive Selected')
    setLang('tr')
    expect(t('smart.emptyTitle')).toBe('Sürücü Seçilmedi')
  })

  it('kw.subtitle matches the full-file content walk, not the old 256 KB / 16 MiB cap', () => {
    for (const lang of ['tr', 'en'] as const) {
      setLang(lang)
      const s = t('kw.subtitle')
      expect(s).not.toMatch(/\(ilk 256 KB\)/)
      expect(s).not.toMatch(/\(first 256 KB\)/)
      expect(s).not.toMatch(/16 MiB üstü dosyalar/)
      expect(s).not.toMatch(/Files over 16 MiB/)
      expect(s).toMatch(/256 KiB/)
      expect(s).toMatch(/64 KiB/)
    }
  })

  it('setLang does not throw without a DOM (lang sync guard)', () => {
    expect(() => setLang('en')).not.toThrow()
    expect(() => setLang('tr')).not.toThrow()
  })
})
