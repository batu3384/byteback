/** Native ContentFinishedCallback status. Keep in sync with content_search.h. */
export const CONTENT_SEARCH_COMPLETE = 1
export const CONTENT_SEARCH_STOPPED = 2
export const CONTENT_SEARCH_OPEN_FAILED = 3
export const CONTENT_SEARCH_QUERY_TOO_LONG = 4
export const CONTENT_SEARCH_REGEX_REJECTED = 5
export const CONTENT_SEARCH_READ_INCOMPLETE = 6
/** Keep in sync with kMaxContentQueryBytes in content_search.h. */
export const CONTENT_SEARCH_MAX_QUERY_BYTES = 64 * 1024
/** Keep in sync with kMaxContentRegexChars in content_search.h. */
export const CONTENT_SEARCH_MAX_REGEX_CHARS = 128

export function isContentSearchOpenFailed(status: number): boolean {
  return status === CONTENT_SEARCH_OPEN_FAILED
}

export function isContentSearchQueryTooLong(status: number): boolean {
  return status === CONTENT_SEARCH_QUERY_TOO_LONG
}

export function isContentSearchRegexRejected(status: number): boolean {
  return status === CONTENT_SEARCH_REGEX_REJECTED
}

export function isContentSearchReadIncomplete(status: number): boolean {
  return status === CONTENT_SEARCH_READ_INCOMPLETE
}

export function contentQueryByteLength(query: string): number {
  return new TextEncoder().encode(query).length
}
