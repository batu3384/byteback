import { describe, it, expect } from 'vitest'
import { csvCell, htmlEscape } from './html-escape'

describe('htmlEscape', () => {
  it('escapes markup and quotes', () => {
    expect(htmlEscape('<b>"x"&\'')).toBe('&lt;b&gt;&quot;x&quot;&amp;&#39;')
  })
})

describe('csvCell', () => {
  it('neutralizes formula injection', () => {
    expect(csvCell('=cmd')).toBe("'=cmd")
    expect(csvCell('+1+1')).toBe("'+1+1")
  })

  it('quotes delimiter characters', () => {
    expect(csvCell('a;b')).toBe('"a;b"')
  })
})
