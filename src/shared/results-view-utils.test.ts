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
  })

  it('confidence tiers color triage chips', () => {
    expect(confidenceTier(90)).toBe('high')
    expect(confidenceTier(60)).toBe('mid')
    expect(confidenceTier(20)).toBe('low')
    expect(confidenceTier(undefined)).toBe('none')
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
})
