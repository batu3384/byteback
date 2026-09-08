import { describe, expect, it } from 'vitest'
import { buildMatchParts } from './highlight'

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
