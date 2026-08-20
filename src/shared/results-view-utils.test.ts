import { describe, expect, it } from 'vitest'
import { buildTree, getFileType, formatSize } from '../renderer/components/ResultsView/results-view-utils'

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
    expect(formatSize(2048)).toBe('2.00 KB')
  })
})
