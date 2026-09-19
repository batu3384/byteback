import { describe, expect, it } from 'vitest'
import {
  HEX_MARKS_CAP,
  HEX_MARKS_FILE,
  addHexMark,
  parseHexMarks,
  removeHexMark,
  serializeHexMarks,
  type HexMark,
} from './hex-marks'

const mark = (over: Partial<HexMark> = {}): HexMark => ({
  driveIndex: 0,
  volumePath: '',
  sector: 8,
  label: '',
  ...over,
})

describe('hex-marks.json (FAZ 4.1 bookmark)', () => {
  it('names the userData sidecar', () => {
    expect(HEX_MARKS_FILE).toBe('hex-marks.json')
  })

  it('parses a valid marks array', () => {
    const raw = JSON.stringify({ marks: [mark({ sector: 16, label: 'boot' })] })
    expect(parseHexMarks(raw)).toEqual([mark({ sector: 16, label: 'boot' })])
  })

  it('returns empty on junk JSON, missing marks, or hostile types', () => {
    expect(parseHexMarks('')).toEqual([])
    expect(parseHexMarks('{')).toEqual([])
    expect(parseHexMarks('{"marks":"nope"}')).toEqual([])
    expect(parseHexMarks(JSON.stringify({ marks: [{ driveIndex: 0.5, sector: 1 }] }))).toEqual([])
    expect(parseHexMarks(JSON.stringify({ marks: [{ driveIndex: 0, sector: -1 }] }))).toEqual([])
  })

  it('dedupes same drive+path+sector and caps at HEX_MARKS_CAP dropping oldest', () => {
    let marks: HexMark[] = []
    marks = addHexMark(marks, mark({ sector: 1 }))
    marks = addHexMark(marks, mark({ sector: 1, label: 'again' }))
    expect(marks).toHaveLength(1)
    expect(marks[0]?.label).toBe('again')

    marks = []
    for (let i = 0; i < HEX_MARKS_CAP + 3; i++) {
      marks = addHexMark(marks, mark({ sector: i }))
    }
    expect(marks).toHaveLength(HEX_MARKS_CAP)
    expect(marks[0]?.sector).toBe(3)
    expect(marks[HEX_MARKS_CAP - 1]?.sector).toBe(HEX_MARKS_CAP + 2)
  })

  it('removeHexMark drops the matching sector and round-trips serialize', () => {
    const a = mark({ sector: 8 })
    const b = mark({ sector: 9, volumePath: '\\\\.\\C:' })
    let marks = addHexMark(addHexMark([], a), b)
    marks = removeHexMark(marks, a)
    expect(marks).toEqual([b])
    expect(parseHexMarks(serializeHexMarks(marks))).toEqual([b])
  })
})
