/** ASCII `\\.\X:` — drive-letter volume device, never PhysicalDrive or UNC. */
export function isWin32VolumeDevicePath(p: string): boolean {
  return /^\\\\\.\\[A-Za-z]:$/.test(p)
}

/** Image the scan-bound volume by default so the clone matches the examined unit. */
export function defaultImagerUseVolume(volumePath?: string | null): boolean {
  return typeof volumePath === 'string' && isWin32VolumeDevicePath(volumePath)
}
