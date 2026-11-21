/** Match native DataShredder::wipeSerialMatches (whitespace strip, case fold). */
export function wipeSerialMatches(typed: string, actual: string): boolean {
  const norm = (s: string) => s.replace(/\s/g, '').toUpperCase()
  const t = norm(typed)
  const a = norm(actual)
  if (!t || !a) return false
  return t === a
}

type VolumeDisks = { driveIndex: number; diskNumbers?: number[] } | null | undefined

/** True when the drive backs a protected volume (Windows, app data). An
 *  unresolved volume counts as protected: its disk is unknown, not safe. */
export function wipeHitsProtectedVolume(driveIndex: number, volumes: VolumeDisks[]): boolean {
  return volumes.some((v) => {
    if (!v) return true
    const disks = v.diskNumbers && v.diskNumbers.length > 0 ? v.diskNumbers : [v.driveIndex]
    return disks.includes(driveIndex)
  })
}
