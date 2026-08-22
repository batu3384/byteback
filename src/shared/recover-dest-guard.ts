/** Extract Windows drive letter from an absolute path, or null. */
export function volumeLetterOfPath(path: string): string | null {
  const m = /^([A-Za-z]):[\\/]/.exec(path.trim())
  return m ? m[1]!.toUpperCase() : null
}

/**
 * True when destination sits on the same physical drive index as the scan.
 * Used to warn before overwrite risk (Recuva/DiskDrill style).
 */
export async function isDestOnScannedDrive(
  destDir: string,
  driveIndex: number,
  resolveVolume: (letter: string) => Promise<{ driveIndex: number } | null | undefined>,
): Promise<boolean> {
  if (driveIndex < 0) return false
  const letter = volumeLetterOfPath(destDir)
  if (!letter) return false
  const resolved = await resolveVolume(letter)
  return resolved?.driveIndex === driveIndex
}
