/** Split text into literal/regex match partitions for KeywordSearch <mark>
 *  highlighting. Pure so it is unit-testable without a DOM. */
import type { FileRecord } from '../../../shared/ipc-contract'

export interface MatchPart {
  text: string
  match: boolean
}

/** Native content-search record extension: FileRecord already carries
 *  `snippet`; the match offsets land alongside it (number, -1 = none) when the
 *  native lane lands. Declared here so the UI can render defensively today. */
export interface SnippetRecord extends FileRecord {
  snippetMatchStart?: number
  snippetMatchEnd?: number
}

/** UTF-8 encoded length of one code point (Math.ceil(log…) as a table). */
function utf8ByteLen(cp: number): number {
  if (cp < 0x80) return 1
  if (cp < 0x800) return 2
  if (cp < 0x10000) return 3
  return 4
}

/** Native snippet offsets are BYTE offsets into the sanitized UTF-8 context
 *  (content_search.cpp attachSnippet), but JS string indices are UTF-16 code
 *  units — "şğİ"/emoji shift every later highlight. Binary-context snippets
 *  are pure ASCII ('.' replacements) so bytes map 1:1; text-context snippets
 *  are valid UTF-8 (utf8_sanitize.h utf8ForJs enforces it), so the map below
 *  is exact. Residual corner: an invalid sequence replaced by '?' shifts the
 *  native byte count — such offsets usually miss a boundary and degrade here.
 *  Returns -1 when the byte offset does not land on a code-point boundary
 *  inside `text` (includes offsets past the end). */
function byteOffsetToUnitIndex(text: string, byteOffset: number): number {
  if (byteOffset === 0) return 0
  let bytes = 0
  let units = 0
  for (const ch of text) {
    bytes += utf8ByteLen(ch.codePointAt(0) as number)
    units += ch.length
    if (bytes === byteOffset) return units
    if (bytes > byteOffset) return -1
  }
  return -1
}

/** Partition a content-search snippet so the [start,end) match span renders as
 *  a <mark>. Defensive: missing/-1/non-integer/out-of-bounds indices return the
 *  snippet unhighlighted instead of slicing wrong. `start`/`end` are native
 *  BYTE offsets (see byteOffsetToUnitIndex) and are converted before slicing;
 *  unconvertible offsets degrade to no highlight — never to a wrong mark. Pure
 *  and unit-testable. */
export function buildSnippetParts(snippet: string, start?: number, end?: number): MatchPart[] {
  if (!snippet) return []
  const rawStart = typeof start === 'number' ? start : -1
  const rawEnd = typeof end === 'number' ? end : -1
  if (!Number.isInteger(rawStart) || !Number.isInteger(rawEnd) || rawStart < 0 || rawEnd <= rawStart) {
    return [{ text: snippet, match: false }]
  }
  let s = rawStart
  let e = rawEnd
  if (/[^\u0000-\u007F]/.test(snippet)) {
    // Non-ASCII snippet: byte offsets drift from code units — convert exactly
    // or do not highlight at all (a drifted slice marks the wrong characters).
    s = byteOffsetToUnitIndex(snippet, rawStart)
    e = byteOffsetToUnitIndex(snippet, rawEnd)
    if (s < 0 || e < 0) return [{ text: snippet, match: false }]
  }
  if (e > snippet.length) return [{ text: snippet, match: false }]
  const parts: MatchPart[] = []
  if (s > 0) parts.push({ text: snippet.slice(0, s), match: false })
  parts.push({ text: snippet.slice(s, e), match: true })
  if (e < snippet.length) parts.push({ text: snippet.slice(e), match: false })
  return parts
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

/** Conservative nested-quantifier check for user-supplied highlight regexes.
 *  A quantifier applied to a group that itself contains a quantifier or an
 *  alternation can backtrack catastrophically — "(a+)+$" against a long
 *  non-matching filename freezes the renderer per row (V8 has no match
 *  timeout). Known residual limits, accepted because the failure mode is only
 *  "no highlight" (never wrong output): a quantified alternation is rejected
 *  even when its branches are disjoint — e.g. "(jpg|png)+" — and backreference
 *  ambiguity is not analysed. Ordinary alternation without an outer
 *  quantifier, e.g. "(jpg|png)$", stays highlighted. Pure and unit-testable. */
export function isSafeHighlightRegex(pattern: string): boolean {
  interface Frame { quant: boolean; alt: boolean; innerRisk: boolean }
  const stack: Frame[] = [{ quant: false, alt: false, innerRisk: false }]
  let inClass = false
  let prevGroupClose = false // last significant atom was ")"
  let lastPopped: Frame | null = null
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i]
    if (c === '\\') { i++; prevGroupClose = false; continue }
    if (inClass) {
      if (c === ']') inClass = false
      continue
    }
    if (c === '[') { inClass = true; prevGroupClose = false; continue }
    if (c === '(') {
      stack.push({ quant: false, alt: false, innerRisk: false })
      // Group syntax prefixes — "(?:", "(?=", "(?!", "(?<" — are not
      // quantifiers even though some contain '*', '+' or '?' characters.
      if (pattern[i + 1] === '?') {
        i++ // consume '?'
        const n = pattern[i + 1]
        if (n === ':' || n === '=' || n === '!') i++
      }
      prevGroupClose = false
      continue
    }
    if (c === ')') {
      const f = stack.pop()
      if (!f) return false // unbalanced — compile will fail anyway
      lastPopped = f
      const parent = stack[stack.length - 1]
      if (parent) parent.innerRisk = parent.innerRisk || f.quant || f.alt || f.innerRisk
      prevGroupClose = true
      continue
    }
    if (c === '|') { stack[stack.length - 1]!.alt = true; prevGroupClose = false; continue }
    const isQuantifier = c === '*' || c === '+' || c === '?' ||
      (c === '{' && /^\{\d+(,\d*)?\}/.test(pattern.slice(i)))
    if (isQuantifier) {
      if (c === '{') i += pattern.slice(i).match(/^\{\d+(,\d*)?\}/)![0].length - 1
      const cur = stack[stack.length - 1]!
      if (prevGroupClose && lastPopped && (lastPopped.quant || lastPopped.alt || lastPopped.innerRisk)) {
        return false
      }
      cur.quant = true
      prevGroupClose = false
      continue
    }
    prevGroupClose = false
  }
  return stack.length === 1
}

export function buildMatchParts(text: string, query: string, useRegex: boolean): MatchPart[] {
  if (!text || !query.trim()) return text ? [{ text, match: false }] : []

  let re: RegExp | null = null
  try {
    if (useRegex) {
      // Backtracking bomb: refuse to execute — degrade to plain text.
      if (!isSafeHighlightRegex(query)) return [{ text, match: false }]
      re = new RegExp(query, 'gi')
    } else {
      re = new RegExp(escapeRegExp(query), 'gi')
    }
  } catch {
    // Invalid regex (UI validates upfront, but stay safe): no highlighting.
    return [{ text, match: false }]
  }

  const parts: MatchPart[] = []
  let last = 0
  for (const m of text.matchAll(re)) {
    const idx = m.index ?? 0
    if (m[0].length === 0) break // zero-width match guard (e.g. "a*")
    if (idx > last) parts.push({ text: text.slice(last, idx), match: false })
    parts.push({ text: m[0], match: true })
    last = idx + m[0].length
  }
  if (parts.length === 0) return [{ text, match: false }]
  if (last < text.length) parts.push({ text: text.slice(last), match: false })
  return parts
}
