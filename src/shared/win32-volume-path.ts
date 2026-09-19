/** ASCII `\\.\X:` — drive-letter volume device, never PhysicalDrive or UNC. */
export function isWin32VolumeDevicePath(p: string): boolean {
  return /^\\\\\.\\[A-Za-z]:$/.test(p)
}

/** Local evidence file path — not a volume device, PhysicalDrive, http(s), or /dev/. */
export function isEvidenceImagePath(p: unknown): p is string {
  if (typeof p !== 'string' || p.length === 0) return false
  if (isWin32VolumeDevicePath(p)) return false
  if (p.startsWith('\\\\.\\')) return false
  if (/^https?:\/\//i.test(p)) return false
  if (p.startsWith('/dev/')) return false
  return true
}

/** Image the scan-bound volume or evidence file by default so the clone matches the examined unit. */
export function defaultImagerUseVolume(volumePath?: string | null): boolean {
  return typeof volumePath === 'string'
    && (isWin32VolumeDevicePath(volumePath) || isEvidenceImagePath(volumePath))
}

/** Resolve startImaging args: evidence file uses driveIndex -2; volume device keeps PhysicalDrive + \\.\X:. */
export function imagerStartArgs(
  selectedDrive: number | '',
  destPath: string,
  useVolume: boolean,
  scanVolumePath?: string,
): { driveIndex: number; volumePath?: string } | null {
  if (!destPath.trim()) return null
  if (useVolume && isEvidenceImagePath(scanVolumePath)) {
    return { driveIndex: -2, volumePath: scanVolumePath }
  }
  if (selectedDrive === '' || selectedDrive === undefined) return null
  const driveIndex = Number(selectedDrive)
  if (!Number.isInteger(driveIndex)) return null
  if (driveIndex === -1) return { driveIndex }
  const vp = useVolume && isWin32VolumeDevicePath(scanVolumePath ?? '') ? scanVolumePath : undefined
  return vp ? { driveIndex, volumePath: vp } : { driveIndex }
}
