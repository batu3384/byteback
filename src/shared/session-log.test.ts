import { describe, expect, it } from 'vitest'
import { parseSessionEvent, sessionLogCode, summarizeSessionLines } from './session-log'

describe('session-log', () => {
  it('parses event token after timestamp', () => {
    expect(parseSessionEvent('2026-08-21T12:00:00.000Z SCAN_START drive=0')).toBe('SCAN_START')
    expect(parseSessionEvent('')).toBe('')
  })

  it('explains close during live scan', () => {
    const lines = [
      '2026-08-21T12:00:00.000Z APP_START',
      '2026-08-21T12:01:00.000Z SCAN_START scanId=3 type=deep',
      '2026-08-21T12:10:00.000Z SCAN_PROGRESS current=100 total=1000',
      '2026-08-21T12:11:00.000Z WINDOW_CLOSE scan_live=1',
      '2026-08-21T12:11:01.000Z APP_QUIT',
    ]
    expect(summarizeSessionLines(lines)).toContain('tarama bitmeden kapandı')
  })

  it('explains completed scan', () => {
    const lines = [
      't SCAN_START',
      't SCAN_COMPLETE status=1',
    ]
    expect(summarizeSessionLines(lines)).toBe('Son tarama tamamlandı.')
  })

  it('says live while last line is progress', () => {
    expect(summarizeSessionLines(['t SCAN_START', 't SCAN_PROGRESS current=1'])).toContain('sürüyor')
  })

  it('explains process restart after live scan', () => {
    const lines = ['t SCAN_START', 't SCAN_PROGRESS', 't APP_START']
    expect(summarizeSessionLines(lines)).toContain('tarama bitmeden kapandı')
  })

  it('classifies sessions with machine codes (renderer keys off these, not TR substrings)', () => {
    expect(sessionLogCode([])).toBe('no_scan')
    expect(sessionLogCode(['t RENDER_GONE'])).toBe('crash')
    expect(sessionLogCode(['t SCAN_START', 't SCAN_COMPLETE'])).toBe('complete')
    expect(sessionLogCode(['t SCAN_START', 't SCAN_FAIL'])).toBe('fail')
    expect(sessionLogCode(['t SCAN_START', 't SCAN_STOP'])).toBe('stopped')
    expect(sessionLogCode(['t SCAN_START', 't SCAN_PROGRESS'])).toBe('running')
    expect(sessionLogCode(['t SCAN_START', 't CRASH'])).toBe('crash_during_scan')
    expect(sessionLogCode(['t SCAN_START', 't OS_SLEEP'])).toBe('sleep')
    expect(sessionLogCode(['t SCAN_START', 't WINDOW_CLOSE'])).toBe('quit_early')
    expect(sessionLogCode(['t SCAN_START'])).toBe('running')
    expect(sessionLogCode(['t SCAN_START', 't MYSTERY_EVENT'])).toBe('incomplete')
  })
})
