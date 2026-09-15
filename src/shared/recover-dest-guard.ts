/** Strip Win32 extended-path prefix so drive letter parsing works. */
export function normalizeWindowsPath(path: string): string {
  let p = path.trim()
  if (p.startsWith('\\\\?\\')) {
    p = p.slice(4)
    if (p.startsWith('UNC\\')) return '\\\\' + p.slice(4)
  }
  return p
}

/** Extract Windows drive letter from an absolute path, or null. */
export function volumeLetterOfPath(path: string): string | null {
  const normalized = normalizeWindowsPath(path)
  const m = /^([A-Za-z]):[\\/]/.exec(normalized)
  return m ? m[1]!.toUpperCase() : null
}

export type VolumeResolver = (letter: string) => Promise<{ driveIndex: number; diskNumbers?: number[] } | null | undefined>

function disksOf(vol: { driveIndex: number; diskNumbers?: number[] }): number[] {
  if (vol.diskNumbers && vol.diskNumbers.length > 0) {
    return vol.diskNumbers.filter((n) => Number.isInteger(n) && n >= 0)
  }
  return vol.driveIndex >= 0 ? [vol.driveIndex] : []
}

/**
 * True when destination sits on the same physical drive index as the scan.
 * Used to warn before overwrite risk (Recuva/DiskDrill style).
 */
export async function isDestOnScannedDrive(
  destDir: string,
  driveIndex: number,
  resolveVolume: VolumeResolver,
): Promise<boolean> {
  if (driveIndex < 0) return false
  const letter = volumeLetterOfPath(destDir)
  if (!letter) return false
  const resolved = await resolveVolume(letter)
  if (resolved == null) return false
  return disksOf(resolved).includes(driveIndex)
}

/** RAID recover: warn when dest sits on any member disk of the virtual array. */
export async function isDestOnRaidMemberDrive(
  destDir: string,
  memberDriveIndices: number[],
  resolveVolume: VolumeResolver,
): Promise<boolean> {
  if (memberDriveIndices.length === 0) return false
  const letter = volumeLetterOfPath(destDir)
  if (!letter) return false
  const resolved = await resolveVolume(letter)
  if (resolved == null) return false
  const destDisks = disksOf(resolved)
  return destDisks.some((d) => memberDriveIndices.includes(d))
}

export type VolumeLookup = (letter: string) => { driveIndex: number; diskNumbers?: number[] } | null | undefined

/** Sync variant for main-process recover IPC (resolveVolume is not async there). */
export function isDestOnEvidence(
  destDir: string,
  scannedDriveIndex: number,
  raidMemberIndices: readonly number[],
  resolveVolume: VolumeLookup,
): boolean {
  const letter = volumeLetterOfPath(destDir)
  if (!letter) return false
  const vol = resolveVolume(letter)
  if (vol == null) return false
  const destDisks = disksOf(vol)
  if (scannedDriveIndex >= 0 && destDisks.includes(scannedDriveIndex)) return true
  return destDisks.some((d) => raidMemberIndices.includes(d))
}
