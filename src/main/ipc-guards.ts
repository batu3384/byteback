import { SCAN_IMAGE_DRIVE_INDEX } from '../shared/hex-read'

export function assertDriveIndex(v: unknown): void {
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 0) {
    throw new Error('Geçersiz sürücü indeksi')
  }
}

export function assertBoundDriveIndex(v: unknown): void {
  if (v === -1 || v === SCAN_IMAGE_DRIVE_INDEX) return
  assertDriveIndex(v)
}
