export const CRASH_DUMPS_DIR_NAME = 'CrashDumps'

export type CrashDumpEntry = { name: string; mtimeMs: number }

export function latestCrashDump(entries: CrashDumpEntry[]): CrashDumpEntry | null {
  let best: CrashDumpEntry | null = null
  for (const e of entries) {
    if (!e.name.toLowerCase().endsWith('.dmp')) continue
    if (!best || e.mtimeMs > best.mtimeMs) best = e
  }
  return best
}
