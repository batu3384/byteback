import { describe, expect, it } from 'vitest'
import {
  CONTENT_SEARCH_COMPLETE,
  CONTENT_SEARCH_OPEN_FAILED,
  CONTENT_SEARCH_QUERY_TOO_LONG,
  CONTENT_SEARCH_REGEX_REJECTED,
  CONTENT_SEARCH_READ_INCOMPLETE,
  CONTENT_SEARCH_STOPPED,
  contentQueryByteLength,
  isContentSearchOpenFailed,
  isContentSearchQueryTooLong,
  isContentSearchRegexRejected,
  isContentSearchReadIncomplete,
  CONTENT_SEARCH_MAX_QUERY_BYTES,
  CONTENT_SEARCH_MAX_REGEX_CHARS,
} from './content-search-status'

describe('content-search-status', () => {
  it('treats only open-failed as a bind error, not empty hits', () => {
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_OPEN_FAILED)).toBe(true)
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_COMPLETE)).toBe(false)
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_STOPPED)).toBe(false)
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_QUERY_TOO_LONG)).toBe(false)
  })

  it('flags oversized queries without treating them as empty hits', () => {
    expect(isContentSearchQueryTooLong(CONTENT_SEARCH_QUERY_TOO_LONG)).toBe(true)
    expect(isContentSearchQueryTooLong(CONTENT_SEARCH_COMPLETE)).toBe(false)
    expect(contentQueryByteLength('é')).toBe(2)
    expect(CONTENT_SEARCH_MAX_QUERY_BYTES).toBe(64 * 1024)
  })

  it('flags refused regex without treating it as empty hits', () => {
    expect(isContentSearchRegexRejected(CONTENT_SEARCH_REGEX_REJECTED)).toBe(true)
    expect(isContentSearchRegexRejected(CONTENT_SEARCH_COMPLETE)).toBe(false)
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_REGEX_REJECTED)).toBe(false)
    expect(CONTENT_SEARCH_MAX_REGEX_CHARS).toBe(128)
  })

  it('flags unread I/O without treating it as a clean complete', () => {
    expect(isContentSearchReadIncomplete(CONTENT_SEARCH_READ_INCOMPLETE)).toBe(true)
    expect(isContentSearchReadIncomplete(CONTENT_SEARCH_COMPLETE)).toBe(false)
    expect(isContentSearchOpenFailed(CONTENT_SEARCH_READ_INCOMPLETE)).toBe(false)
    expect(CONTENT_SEARCH_READ_INCOMPLETE).toBe(6)
  })
})
