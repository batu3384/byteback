import { describe, expect, it } from 'vitest'
import {
  buildTree,
  getFileType,
  formatSize,
  formatFsTimestamp,
  resolveFileTypeChip,
  statusDisplayLabel,
  qualityHint,
  chipToCategory,
  toSqlListFilter,
  sortKey,
  confidenceTier,
  cursorValueFor,
} from '../renderer/components/ResultsView/results-view-utils'
import type { FileRecord } from './ipc-contract'

describe('results-view-utils', () => {
  it('buildTree nests files under directories', () => {
    const tree = buildTree([
      {
        id: 1,
        name: 'a.txt',
        rawPath: 'docs/a.txt',
        rawStatus: 0,
        size: '1 KB',
        path: 'docs/a.txt',
        type: 'doc',
        status: 'Silinmiş / unallocated',
        statusKey: 'status.deleted',
        sourceLabel: 'ntfs_mft',
        dateLabel: '—',
        qualityLabel: '—',
        confidenceTier: 'none',
      },
    ])
    expect(tree.dirs.has('docs')).toBe(true)
    expect(tree.dirs.get('docs')!.files[0]?.name).toBe('a.txt')
  })

  it('buildTree keeps bare filename at root', () => {
    const tree = buildTree([
      {
        id: 2,
        name: 'alone.bin',
        rawPath: 'alone.bin',
        rawStatus: 0,
        size: '1 B',
        path: 'alone.bin',
        type: 'other',
        status: 'Oyulmuş (imza)',
        statusKey: 'status.carved',
        sourceLabel: 'carver',
        dateLabel: 'FS tarihi yok',
        qualityLabel: 'Zayıf',
        confidenceTier: 'low',
      },
    ])
    expect(tree.dirs.size).toBe(0)
    expect(tree.files[0]?.name).toBe('alone.bin')
  })

  it('classifies extensions and formats size', () => {
    expect(getFileType('png')).toBe('img')
    expect(getFileType('heic')).toBe('img')
    expect(formatSize(2048)).toBe('2.00 KB')
  })

  it('prefers category over misleading name extension', () => {
    expect(resolveFileTypeChip({ name: 'x.bin', category: 'Image', extension: 'jpg' })).toBe('img')
  })

  it('formats zero timestamps honestly', () => {
    expect(formatFsTimestamp(0)).toBe('—')
    expect(formatFsTimestamp(0, 'carver')).toBe('FS tarihi yok')
    expect(formatFsTimestamp(undefined, 'thumbcache')).toBe('—')
    expect(formatFsTimestamp(1_700_000_000)).toMatch(/\d/)
    expect(formatFsTimestamp(1_700_000_000, 'carver')).toMatch(/^EXIF ·/)
  })

  it('separates carve status from metadata deleted', () => {
    expect(statusDisplayLabel(0, 'carver')).toBe('Oyulmuş (imza)')
    expect(statusDisplayLabel(0, 'ntfs_mft')).toBe('Silinmiş / unallocated')
    expect(statusDisplayLabel(1, 'ntfs_mft')).toBe('Tahsisli / kullanımda')
  })

  it('surfaces MFT confidence in quality hint', () => {
    const mft: FileRecord = { id: 1, name: 'a', confidence: 40, source: 'ntfs_mft', sizeBytes: 0, status: 0 }
    const carve: FileRecord = { id: 2, name: 'b', confidence: 90, source: 'carver', sizeBytes: 0, status: 0 }
    expect(qualityHint(mft)).toBe('Düşük güven')
    expect(qualityHint(carve)).toBe('Muhtemelen tam')
  })

  it('sort keys land in the native ORDER BY whitelist', () => {
    expect(sortKey('confidence', 'desc')).toBe('confidence_desc')
    expect(sortKey('name', 'asc')).toBe('name_asc')
    expect(sortKey('id', 'desc')).toBe('') // id = native default
    // Path sort is wired UI-first: keys are stable while the native whitelist
    // gains path_asc/path_desc in a parallel lane (degrades to id order until
    // then, which the engine treats like any unknown orderBy).
    expect(sortKey('path', 'asc')).toBe('path_asc')
    expect(sortKey('path', 'desc')).toBe('path_desc')
  })

  it('confidence tiers color triage chips', () => {
    expect(confidenceTier(90)).toBe('high')
    expect(confidenceTier(60)).toBe('mid')
    expect(confidenceTier(20)).toBe('low')
    expect(confidenceTier(undefined)).toBe('none')
  })

  it('toSqlListFilter passes size/date bounds through (P0-4)', () => {
    const f = toSqlListFilter('all', 'all', '', true, 'confidence_desc',
      { sizeMin: 1024, sizeMax: 1048576, dateFrom: 100, dateTo: 200 })
    expect(f.sizeMin).toBe(1024)
    expect(f.sizeMax).toBe(1048576)
    expect(f.dateFrom).toBe(100)
    expect(f.dateTo).toBe(200)
    expect(f.orderBy).toBe('confidence_desc')

    const bare = toSqlListFilter('all', 'all', '', true)
    expect(bare.sizeMin).toBeUndefined()
    expect(bare.sizeMax).toBeUndefined()
    expect(bare.dateFrom).toBeUndefined()
    expect(bare.dateTo).toBeUndefined()
  })

  it('maps type chips to SQL category', () => {
    expect(chipToCategory('img')).toBe('Image')
    expect(chipToCategory('all')).toBe('')
  })

  it('maps status chips to SQL list filter', () => {
    expect(toSqlListFilter('carved', 'all', '', false)).toEqual({
      status: -1,
      category: '',
      query: '',
      sourceLike: 'carver%',
      sourceNotLike: '',
      includeDuplicates: false,
      includeDiscovery: false,
    })
    expect(toSqlListFilter('deleted', 'img', 'x', true)).toEqual({
      status: 0,
      category: 'Image',
      query: 'x',
      sourceLike: '',
      sourceNotLike: 'carver%',
      includeDuplicates: true,
      includeDiscovery: false,
    })
  })

  // FAZ 1.2: keyset cursor rides inside the filter; absent extra keeps it
  // undefined so the wire payload stays byte-compatible with pre-cursor.
  it('toSqlListFilter carries the keyset cursor (FAZ 1.2)', () => {
    const f = toSqlListFilter('all', 'all', '', true, 'name_asc', { cursor: { v: 'b.txt', id: 12 } })
    expect(f.cursor).toEqual({ v: 'b.txt', id: 12 })
    expect(toSqlListFilter('all', 'all', '', true).cursor).toBeUndefined()
  })

  // FAZ 1.2: cursorValueFor must mirror the native sortKeySql expression per
  // field — including the date CASE (modified_at > created_at ? m : c) in
  // unix SECONDS, tie → createdAt.
  describe('cursorValueFor (FAZ 1.2 keyset)', () => {
    const rec = {
      id: 7,
      name: 'A.wav',
      path: '/Docs/A.wav',
      sizeBytes: 4096,
      confidence: 85,
      createdAt: 100,
      modifiedAt: 90,
    } as FileRecord

    it('mirrors the native sort expressions', () => {
      expect(cursorValueFor('confidence', rec)).toBe(85)
      expect(cursorValueFor('size', rec)).toBe(4096)
      expect(cursorValueFor('name', rec)).toBe('A.wav')
      expect(cursorValueFor('path', rec)).toBe('/Docs/A.wav')
      expect(cursorValueFor('date', rec)).toBe(100)
      expect(cursorValueFor('date', { ...rec, modifiedAt: 120 })).toBe(120)
      expect(cursorValueFor('date', { ...rec, modifiedAt: 100 })).toBe(100)
    })

    it('tolerates missing fields and the non-keyset id field', () => {
      expect(cursorValueFor('confidence', {} as FileRecord)).toBe(0)
      expect(cursorValueFor('size', {} as FileRecord)).toBe(0)
      expect(cursorValueFor('name', {} as FileRecord)).toBe('')
      expect(cursorValueFor('path', {} as FileRecord)).toBe('')
      expect(cursorValueFor('date', {} as FileRecord)).toBe(0)
      expect(cursorValueFor('id', rec)).toBe(0)
    })
  })
})
