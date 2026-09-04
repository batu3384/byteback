import { describe, expect, it, beforeAll } from 'vitest'
import { localizeNote, setLang, getLang, t } from './index'

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
})
