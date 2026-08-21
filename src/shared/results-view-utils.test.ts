import { describe, expect, it } from 'vitest'
import { buildTree, getFileType, formatSize, chipToCategory, toSqlListFilter } from '../renderer/components/ResultsView/results-view-utils'

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
        sourceLabel: 'ntfs_mft',
      },
    ])
    expect(tree.dirs.has('docs')).toBe(true)
    expect(tree.dirs.get('docs')!.files[0]?.name).toBe('a.txt')
  })

  it('classifies extensions and formats size', () => {
    expect(getFileType('png')).toBe('img')
    expect(getFileType('heic')).toBe('img')
    expect(formatSize(2048)).toBe('2.00 KB')
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
      includeDuplicates: false,
      includeDiscovery: false,
    })
    expect(toSqlListFilter('deleted', 'img', 'x', true).status).toBe(0)
    expect(toSqlListFilter('deleted', 'img', 'x', true).category).toBe('Image')
    expect(toSqlListFilter('deleted', 'img', 'x', true).includeDuplicates).toBe(true)
  })
})
