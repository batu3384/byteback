import { describe, it, expect } from 'vitest'
import { contentRegexQueryOk, isSafeHighlightRegex, CONTENT_REGEX_REJECTED_MESSAGE } from './content-regex-query'

describe('contentRegexQueryOk (IPC/native parity)', () => {
  it('rejects catastrophic backtracking shapes', () => {
    expect(contentRegexQueryOk('(a+)+$')).toBe(false)
    expect(isSafeHighlightRegex('(a+)+$')).toBe(false)
  })

  it('accepts ordinary patterns', () => {
    expect(contentRegexQueryOk('invoice\\d+')).toBe(true)
    expect(contentRegexQueryOk('(jpg|png)$')).toBe(true)
  })

  it('rejects overlong patterns', () => {
    expect(contentRegexQueryOk('a'.repeat(129))).toBe(false)
  })

  it('uses bridge error literal', () => {
    expect(CONTENT_REGEX_REJECTED_MESSAGE).toBe('regex rejected')
  })
})
