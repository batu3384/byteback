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

/** Partition a content-search snippet so the [start,end) match span renders as
 *  a <mark>. Defensive: missing/-1/non-integer/out-of-bounds indices return the
 *  snippet unhighlighted instead of slicing wrong. Pure and unit-testable. */
export function buildSnippetParts(snippet: string, start?: number, end?: number): MatchPart[] {
  if (!snippet) return []
  const s = typeof start === 'number' ? start : -1
  const e = typeof end === 'number' ? end : -1
  if (!Number.isInteger(s) || !Number.isInteger(e) || s < 0 || e <= s || e > snippet.length) {
    return [{ text: snippet, match: false }]
  }
  const parts: MatchPart[] = []
  if (s > 0) parts.push({ text: snippet.slice(0, s), match: false })
  parts.push({ text: snippet.slice(s, e), match: true })
  if (e < snippet.length) parts.push({ text: snippet.slice(e), match: false })
  return parts
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

export function buildMatchParts(text: string, query: string, useRegex: boolean): MatchPart[] {
  if (!text || !query.trim()) return text ? [{ text, match: false }] : []

  let re: RegExp
  try {
    re = useRegex ? new RegExp(query, 'gi') : new RegExp(escapeRegExp(query), 'gi')
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
