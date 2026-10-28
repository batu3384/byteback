/** Match native DataShredder::wipeSerialMatches (whitespace strip, case fold). */
export function wipeSerialMatches(typed: string, actual: string): boolean {
  const norm = (s: string) => s.replace(/\s/g, '').toUpperCase()
  const t = norm(typed)
  const a = norm(actual)
  if (!t || !a) return false
  return t === a
}
