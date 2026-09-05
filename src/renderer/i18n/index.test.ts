import { describe, expect, it, beforeAll } from 'vitest'
import { localizeNote, setLang, getLang, t, tFormat } from './index'

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
