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
    'report.generateFailed',
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
    'scan.carveSkipped',
    'diskmap.title',
    'diskmap.records',
    'diskmap.deleted',
    'results.loadErrorTitle',
    'results.loadErrorBody',
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
