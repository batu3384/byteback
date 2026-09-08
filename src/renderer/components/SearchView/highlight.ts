/** Split text into literal/regex match partitions for KeywordSearch <mark>
 *  highlighting. Pure so it is unit-testable without a DOM. */

export interface MatchPart {
  text: string
  match: boolean
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
