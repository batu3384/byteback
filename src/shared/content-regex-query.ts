import { CONTENT_SEARCH_MAX_REGEX_CHARS } from './content-search-status'

/** Native bridge + KeywordSearch use this literal on regex guard failure. */
export const CONTENT_REGEX_REJECTED_MESSAGE = 'regex rejected'

/** Conservative nested-quantifier check — mirrors native regexLikelyBacktracking
 *  (content_search.cpp) and renderer highlight policy. */
export function isSafeHighlightRegex(pattern: string): boolean {
  interface Frame { quant: boolean; alt: boolean; innerRisk: boolean }
  const stack: Frame[] = [{ quant: false, alt: false, innerRisk: false }]
  let inClass = false
  let prevGroupClose = false
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
      if (pattern[i + 1] === '?') {
        i++
        const n = pattern[i + 1]
        if (n === ':' || n === '=' || n === '!') i++
      }
      prevGroupClose = false
      continue
    }
    if (c === ')') {
      const f = stack.pop()
      if (!f) return false
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

/** Parity with native contentRegexQueryOk / regexQueryOk before std::regex compile. */
export function contentRegexQueryOk(pattern: string): boolean {
  if (pattern.length > CONTENT_SEARCH_MAX_REGEX_CHARS) return false
  if (!isSafeHighlightRegex(pattern)) return false
  try {
    RegExp(pattern, 'i')
    return true
  } catch {
    return false
  }
}
