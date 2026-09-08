import { describe, expect, it } from 'vitest'
import { buildMatchParts, buildSnippetParts, isSafeHighlightRegex } from './highlight'

describe('buildMatchParts', () => {
  it('splits literal matches case-insensitively', () => {
    expect(buildMatchParts('Fatura_2026.pdf', 'fatura', false)).toEqual([
      { text: 'Fatura', match: true },
      { text: '_2026.pdf', match: false },
    ])
  })

  it('highlights every occurrence', () => {
    expect(buildMatchParts('abcabc', 'abc', false)).toEqual([
      { text: 'abc', match: true },
      { text: 'abc', match: true },
    ])
  })

  it('returns a single non-match part when nothing matches', () => {
    expect(buildMatchParts('report.docx', 'zzz', false)).toEqual([{ text: 'report.docx', match: false }])
  })

  it('escapes regex metacharacters in literal mode', () => {
    // '.' must match a literal dot, not any character.
    expect(buildMatchParts('reportXdocx', 'report.docx', false)).toEqual([{ text: 'reportXdocx', match: false }])
    expect(buildMatchParts('report.docx', 'report.docx', false)).toEqual([
      { text: 'report.docx', match: true },
    ])
  })

  it('supports regex mode', () => {
    expect(buildMatchParts('Invoice2026 final', 'invoice\\d+', true)).toEqual([
      { text: 'Invoice2026', match: true },
      { text: ' final', match: false },
    ])
  })

  it('degrades to no highlighting on an invalid regex', () => {
    expect(buildMatchParts('file.txt', '([unclosed', true)).toEqual([{ text: 'file.txt', match: false }])
  })

  it('guards zero-width regex matches (no hang, no empty marks)', () => {
    // matchAll yields 'a' first, then empty matches at every index — the
    // zero-width break stops before empty <mark>s can render.
    expect(buildMatchParts('abc', 'a*', true)).toEqual([
      { text: 'a', match: true },
      { text: 'bc', match: false },
    ])
    // Leading zero-width match falls back to no highlighting.
    expect(buildMatchParts('abc', 'b*', true)).toEqual([{ text: 'abc', match: false }])
  })

  it('handles empty query and empty text', () => {
    expect(buildMatchParts('file.txt', '', false)).toEqual([{ text: 'file.txt', match: false }])
    expect(buildMatchParts('', 'x', false)).toEqual([])
  })
})

describe('buildSnippetParts (native content-search snippet)', () => {
  it('marks the [start,end) span and keeps the context around it', () => {
    expect(buildSnippetParts('please wire the 500k transfer', 16, 19)).toEqual([
      { text: 'please wire the ', match: false },
      { text: '500', match: true },
      { text: 'k transfer', match: false },
    ])
  })

  it('handles a match at the very start and end', () => {
    expect(buildSnippetParts('invoice', 0, 7)).toEqual([{ text: 'invoice', match: true }])
  })

  it('degrades to plain text on missing or -1 offsets (native "none" sentinel)', () => {
    expect(buildSnippetParts('no offsets', undefined, undefined)).toEqual([{ text: 'no offsets', match: false }])
    expect(buildSnippetParts('no offsets', -1, -1)).toEqual([{ text: 'no offsets', match: false }])
  })

  it('degrades to plain text on inverted or out-of-bounds offsets', () => {
    expect(buildSnippetParts('abc', 2, 2)).toEqual([{ text: 'abc', match: false }])
    expect(buildSnippetParts('abc', 3, 1)).toEqual([{ text: 'abc', match: false }])
    expect(buildSnippetParts('abc', 0, 99)).toEqual([{ text: 'abc', match: false }])
    expect(buildSnippetParts('abc', 0.5, 2)).toEqual([{ text: 'abc', match: false }])
  })

  it('returns no parts for an empty snippet', () => {
    expect(buildSnippetParts('', 0, 1)).toEqual([])
  })

  it('converts native BYTE offsets to UTF-16 indices (Turkish İ is 2 UTF-8 bytes)', () => {
    // 'İ' = U+0130 = 2 UTF-8 bytes; match 'final' starts at byte 2, ends at 7.
    // A raw slice would mark 'inal ' (byte drift) — conversion marks 'final'.
    expect(buildSnippetParts('İfinal x', 2, 7)).toEqual([
      { text: 'İ', match: false },
      { text: 'final', match: true },
      { text: ' x', match: false },
    ])
  })

  it('converts offsets past an astral character (emoji = 4 bytes / 2 units)', () => {
    expect(buildSnippetParts('🎯match end', 4, 9)).toEqual([
      { text: '🎯', match: false },
      { text: 'match', match: true },
      { text: ' end', match: false },
    ])
  })

  it('degrades to plain text when a byte offset lands mid-code-point', () => {
    // byte 1 is inside the 2-byte 'ş' — highlighting would mark the wrong span.
    expect(buildSnippetParts('şabc', 1, 4)).toEqual([{ text: 'şabc', match: false }])
  })

  it('degrades to plain text when a byte offset is past the UTF-8 end', () => {
    // 'şş' encodes to 4 bytes; offsets 5..6 point beyond the context.
    expect(buildSnippetParts('şş', 5, 6)).toEqual([{ text: 'şş', match: false }])
  })
})

describe('isSafeHighlightRegex (renderer ReDoS guard)', () => {
  it('rejects nested-quantifier backtracking bombs', () => {
    expect(isSafeHighlightRegex('(a+)+$')).toBe(false)
    expect(isSafeHighlightRegex('(a*)*')).toBe(false)
    expect(isSafeHighlightRegex('(a{1,5})+$')).toBe(false)
    expect(isSafeHighlightRegex('((a+)+b)')).toBe(false)
  })

  it('rejects quantified ambiguous alternations', () => {
    expect(isSafeHighlightRegex('(a|aa)+$')).toBe(false)
    expect(isSafeHighlightRegex('^(a|a?)+$')).toBe(false)
  })

  it('rejects bombs hidden behind non-capturing group syntax', () => {
    expect(isSafeHighlightRegex('(?:a+)+$')).toBe(false)
  })

  it('keeps ordinary regexes highlighted', () => {
    expect(isSafeHighlightRegex('invoice\\d+')).toBe(true)
    expect(isSafeHighlightRegex('a*')).toBe(true)
    expect(isSafeHighlightRegex('(jpg|png)$')).toBe(true) // alternation without outer quantifier
    expect(isSafeHighlightRegex('(?:abc)+')).toBe(true)
    expect(isSafeHighlightRegex('[a+]*')).toBe(true) // quantifier chars inside a class are literal
    expect(isSafeHighlightRegex('draft(_v2)?')).toBe(true)
  })

  it('degrades bomb queries to no highlighting instead of hanging the renderer', () => {
    // If the guard regressed, this input backtracks catastrophically (minutes+).
    const bombText = 'a'.repeat(64) + 'b'
    expect(buildMatchParts(bombText, '(a+)+$', true)).toEqual([{ text: bombText, match: false }])
    expect(buildMatchParts(bombText, '(?:a+)+$', true)).toEqual([{ text: bombText, match: false }])
  })
})
