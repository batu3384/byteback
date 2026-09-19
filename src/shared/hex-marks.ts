import { isHexDriveIndex } from './hex-read'

export const HEX_MARKS_FILE = 'hex-marks.json'
export const HEX_MARKS_CAP = 64

export interface HexMark {
  driveIndex: number
  volumePath: string
  sector: number
  label: string
}

function sameMark(a: HexMark, b: HexMark): boolean {
  return a.driveIndex === b.driveIndex && a.volumePath === b.volumePath && a.sector === b.sector
}

function asMark(v: unknown): HexMark | null {
  if (!v || typeof v !== 'object') return null
  const o = v as Record<string, unknown>
  if (!isHexDriveIndex(o.driveIndex)) return null
  if (typeof o.sector !== 'number' || !Number.isInteger(o.sector) || o.sector < 0) return null
  const volumePath = o.volumePath == null ? '' : o.volumePath
  if (typeof volumePath !== 'string') return null
  const label = o.label == null ? '' : o.label
  if (typeof label !== 'string') return null
  return { driveIndex: o.driveIndex, volumePath, sector: o.sector, label }
}

export function parseHexMarks(raw: string): HexMark[] {
  try {
    const parsed = JSON.parse(raw) as unknown
    if (!parsed || typeof parsed !== 'object') return []
    const marks = (parsed as { marks?: unknown }).marks
    if (!Array.isArray(marks)) return []
    const out: HexMark[] = []
    for (const item of marks) {
      const m = asMark(item)
      if (m) out.push(m)
    }
    return out.slice(-HEX_MARKS_CAP)
  } catch {
    return []
  }
}

export function addHexMark(marks: HexMark[], mark: HexMark): HexMark[] {
  const next = asMark(mark)
  if (!next) return marks.slice()
  const without = marks.filter((m) => !sameMark(m, next))
  without.push(next)
  return without.slice(-HEX_MARKS_CAP)
}

export function removeHexMark(marks: HexMark[], mark: HexMark): HexMark[] {
  return marks.filter((m) => !sameMark(m, mark))
}

export function serializeHexMarks(marks: HexMark[]): string {
  const clean: HexMark[] = []
  for (const item of marks) {
    const m = asMark(item)
    if (m) clean.push(m)
  }
  return JSON.stringify({ marks: clean.slice(-HEX_MARKS_CAP) })
}
