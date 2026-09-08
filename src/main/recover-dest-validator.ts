import { realpathSync } from 'fs'
import { basename, dirname, resolve } from 'path'

/**
 * Main-process defense in depth for recovery destinations (recover-file /
 * recover-files-batch). The native `destDirIsSafe` (recovery/path_util.cpp) is
 * a lexical prefix check and can be bypassed with:
 *   - `\\?\` extended-length prefixes          (\\?\C:\Windows\System32)
 *   - 8.3 short names                          (C:\PROGRA~1)
 *   - missing user-level persistence locations (%APPDATA%...\Startup)
 * This validator runs in main BEFORE the path crosses into native.
 *
 * Policy mirrors the imaging dest pattern (blocklist of system + persistence
 * locations), plus structural hardening:
 *   - only absolute local drive paths (no UNC \\server\share, no \\.\ / \\?\
 *     device paths, no relative paths)
 *   - `\\?\` prefixes are stripped before comparison; comparisons are
 *     case-insensitive
 *   - drive roots stay allowed (user recovers to e.g. D:\Recovered)
 *   - paths are resolved through path.resolve + fs.realpathSync.native (with a
 *     walk-up over not-yet-existing segments) so 8.3 / subst / symlink aliases
 *     collapse onto their real target before the blocklist check
 *
 * NOTE: paths inside the scanned evidence source are NOT rejected here — the
 * source is known only as a driveIndex and mapping it to a volume letter is
 * not cheaply available in main (native listDrives would be needed). The
 * native layer still refuses to open the source device for writing.
 */

export type RecoverDestVerdict =
  | { ok: true; destDir: string }
  | { ok: false; error: string }

/** Injectable environment roots — tests pass explicit temp dirs instead of the real profile. */
export interface RecoverDestEnv {
  systemRoot?: string
  programFiles?: string
  programFilesX86?: string
  appData?: string
  programData?: string
}

export const ERR_DEST_REQUIRED = 'Hedef klasör seçilmedi'
export const ERR_DEST_NOT_ABSOLUTE = 'Hedef klasör reddedildi: mutlak yerel Windows yolu gerekli'
export const ERR_DEST_UNRESOLVED = 'Hedef klasör çözümlenemedi'
export const ERR_DEST_BLOCKED = 'Hedef klasör reddedildi: sistem veya otomatik başlangıç konumu'

function stripExtendedPrefix(raw: string): { path: string; unc: boolean } {
  const p = raw.trim()
  if (/^\\\\\?\\UNC\\/i.test(p)) {
    // \\?\UNC\server\share → \\server\share (still UNC, rejected later)
    return { path: '\\\\' + p.slice(8), unc: true }
  }
  if (/^\\\\\?\\/i.test(p)) {
    // \\?\C:\... → C:\... (extended-length wrapper removed before checks)
    return { path: p.slice(4), unc: false }
  }
  return { path: p, unc: false }
}

function toBackslashes(p: string): string {
  return p.replace(/\//g, '\\')
}

/** Normalize for comparison: backslashes + lower case. */
function canon(p: string): string {
  return toBackslashes(p).toLowerCase()
}

/**
 * path.resolve + fs.realpathSync.native, walking up to the nearest existing
 * ancestor when the leaf does not exist yet (native recovery does
 * create_directories). Returns null only if no ancestor can be resolved
 * (broken path / unavailable volume).
 */
export function strictResolveDest(raw: string): string | null {
  let current = resolve(raw)
  let tail = ''
  for (;;) {
    try {
      return realpathSync.native(current) + tail
    } catch {
      const parent = dirname(current)
      if (parent === current) return null
      tail = '\\' + basename(current) + tail
      current = parent
    }
  }
}

/** True when `resolved` is the blocked root itself or lies underneath it. */
function underRoot(resolvedCanon: string, rootCanon: string): boolean {
  if (!rootCanon) return false
  return resolvedCanon === rootCanon || resolvedCanon.startsWith(rootCanon + '\\')
}

/** Blocklist reason or null when the resolved destination is policy-clean. */
export function isBlockedRecoverDest(resolved: string, env: RecoverDestEnv = {}): string | null {
  const dest = canon(resolved)
  const envOf = (v: string | undefined, fallback: string): string =>
    toBackslashes((v && v.trim()) || fallback)

  const roots: Array<[string, string]> = [
    [envOf(env.systemRoot, process.env.SystemRoot ?? 'C:\\Windows'), 'systemRoot'],
    [envOf(env.programFiles, process.env.ProgramFiles ?? 'C:\\Program Files'), 'programFiles'],
    [envOf(env.programFilesX86, process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)'), 'programFilesX86'],
  ]
  const appData = env.appData ?? process.env.APPDATA
  if (appData && appData.trim()) {
    roots.push([toBackslashes(appData.trim()) + '\\Microsoft\\Windows\\Start Menu\\Programs\\Startup', 'userStartup'])
  }
  const programData = env.programData ?? process.env.ProgramData
  if (programData && programData.trim()) {
    roots.push([toBackslashes(programData.trim()) + '\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp', 'commonStartup'])
  }
  for (const [root, reason] of roots) {
    if (underRoot(dest, canon(root))) return reason
  }
  return null
}

/**
 * Validate a recovery destination coming over IPC. Returns the resolved real
 * path on success — handlers pass it to native instead of the raw input.
 */
export function validateRecoverDestDir(raw: unknown, env: RecoverDestEnv = {}): RecoverDestVerdict {
  if (typeof raw !== 'string' || !raw.trim()) return { ok: false, error: ERR_DEST_REQUIRED }

  const { path: stripped, unc } = stripExtendedPrefix(raw)

  // UNC shares and \\.\ device paths are not writable recovery destinations.
  if (unc || /^\\\\/.test(stripped) || /^\\\\\./.test(raw) || /^\/\//.test(stripped)) {
    return { ok: false, error: ERR_DEST_NOT_ABSOLUTE }
  }
  // Must be an absolute local drive path (X:\...); rejects relative and
  // drive-relative (C:foo) forms that path.resolve would silently rebase.
  if (!/^[A-Za-z]:[\\/]/.test(stripped)) {
    return { ok: false, error: ERR_DEST_NOT_ABSOLUTE }
  }
  // '..' segments must not survive: resolve() would collapse them silently.
  if (toBackslashes(stripped).split('\\').some((seg) => seg === '..')) {
    return { ok: false, error: ERR_DEST_NOT_ABSOLUTE }
  }

  const resolved = strictResolveDest(stripped)
  if (!resolved) return { ok: false, error: ERR_DEST_UNRESOLVED }

  if (isBlockedRecoverDest(resolved, env)) return { ok: false, error: ERR_DEST_BLOCKED }

  return { ok: true, destDir: resolved }
}
