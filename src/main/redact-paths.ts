/**
 * Redact absolute, user-identifying path prefixes from log text. CRASH lines
 * keep their stack (debuggability) but userData and cwd are replaced with
 * placeholders so session.log does not embed C:\Users\<name> on every line.
 *
 * Handles the three shapes that appear in Node/Electron output:
 *   C:\Users\me\app\...      (backslash, as printed by Error.stack)
 *   C:/Users/me/app/...      (forward slash, as printed by some URLs)
 *   file:///C:/Users/me/...  (URI form from module URLs)
 */
export function redactPaths(text: string, paths: { userData: string; cwd: string }): string {
  let out = text
  const entries: Array<[string, string]> = [
    [paths.userData, '<userData>'],
    [paths.cwd, '<cwd>'],
  ]
  for (const [base, tag] of entries) {
    if (!base) continue
    const forward = base.replace(/\\/g, '/')
    const variants = new Set<string>([base, forward, 'file:///' + forward])
    for (const variant of variants) {
      out = out.split(variant).join(tag)
    }
  }
  return out
}
