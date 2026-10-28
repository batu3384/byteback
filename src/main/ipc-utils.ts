import { SCAN_IMAGE_DRIVE_INDEX } from '../shared/hex-read'
import { isEvidenceImagePath } from '../shared/win32-volume-path'

export function startScanDrivePath(driveIndex: number, imagePath?: string): string {
  if (imagePath) return 'image'
  if (driveIndex === -1) return 'raid'
  return String(driveIndex)
}

/** Clamp renderer-supplied numeric args to a safe int range (NaN/±Infinity → fallback). */
export function clampInt(v: unknown, min: number, max: number, fallback: number): number {
  const n = typeof v === 'number' ? v : Number(v)
  if (!Number.isFinite(n)) return fallback
  return Math.min(max, Math.max(min, Math.floor(n)))
}

/** FAZ 1.3c: keep the renderer's suggested CSV file NAME only — never a path. */
export function sanitizeCsvFileName(suggested: unknown): string {
  const fallback = `byteback-sonuclar-${new Date().toISOString().slice(0, 10)}.csv`
  if (typeof suggested !== 'string') return fallback
  const cleaned = suggested.replace(/[\\/:*?"<>|\u0000-\u001f]/g, '').trim()
  if (!cleaned || !cleaned.toLowerCase().endsWith('.csv')) return fallback
  return cleaned.slice(0, 180)
}

export function scanProgressPayload(scanId: number, data: any): Record<string, unknown> {
  return {
    scanId,
    current: data.current,
    total: data.total,
    badSectors: data.badSectors,
    phase: data.phase,
    phaseCurrent: data.phaseCurrent,
    phaseTotal: data.phaseTotal,
  }
}

export function sanitizeRaidIndices(v: unknown, max: number): number[] {
  if (!Array.isArray(v)) return []
  const valid = v.filter((d): d is number => Number.isInteger(d) && d >= 0)
  if (valid.length > max) return []
  return [...new Set(valid)]
}

export function sanitizeRaidImagePaths(v: unknown, max: number): string[] {
  if (!Array.isArray(v)) return []
  const valid = v.filter((p): p is string => typeof p === 'string' && isEvidenceImagePath(p))
  if (valid.length > max) return []
  return [...new Set(valid)]
}
