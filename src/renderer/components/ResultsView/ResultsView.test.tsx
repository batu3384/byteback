// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render, waitFor } from '@testing-library/react'
import ResultsView from './ResultsView'
import type { FileRecord } from '../../../shared/ipc-contract'

// FAZ 1.2: the pager must ride the keyset cursor — every sequential page
// fetch carries the previous page's last row (sort-key value + id) inside
// filter.cursor, and a sort change clears it (fresh page-0 fetch, then a
// cursor rebuilt from the new sort's last row).

const PAGE_A: FileRecord[] = [
  { id: 101, name: 'a.bin', path: '/x/a.bin', status: 0, sizeBytes: 2048, confidence: 90, source: 'mft', createdAt: 100, modifiedAt: 110 } as FileRecord,
  { id: 102, name: 'b.bin', path: '/x/b.bin', status: 0, sizeBytes: 4096, confidence: 70, source: 'mft', createdAt: 200, modifiedAt: 190 } as FileRecord,
]
const PAGE_B: FileRecord[] = [
  { id: 51, name: 'c.bin', path: '/x/c.bin', status: 0, sizeBytes: 8, confidence: 60, source: 'mft', createdAt: 300, modifiedAt: 300 } as FileRecord,
  { id: 52, name: 'd.bin', path: '/x/d.bin', status: 0, sizeBytes: 16, confidence: 40, source: 'mft', createdAt: 400, modifiedAt: 395 } as FileRecord,
]

function mockApi() {
  // Native simulation: return rows ordered by the requested sort key, so the
  // cursor under test really is "the last row of the delivered page".
  const sortRows = (rows: FileRecord[], orderBy?: string): FileRecord[] => {
    const out = [...rows]
    if (orderBy === 'confidence_asc') out.sort((a, b) => (a.confidence ?? 0) - (b.confidence ?? 0))
    else if (orderBy === 'confidence_desc') out.sort((a, b) => (b.confidence ?? 0) - (a.confidence ?? 0))
    return out
  }
  const getFilesPage = vi.fn(
    async (_scanId: number, offset: number, _limit: number, filter?: { orderBy?: string }) =>
      sortRows(offset === 0 ? PAGE_A : PAGE_B, filter?.orderBy),
  )
  ;(window as unknown as { api: unknown }).api = {
    getFilesPage,
    getFileCount: vi.fn(async () => 1000),
    searchFiles: vi.fn(async () => ({ rows: [] })),
  }
  return getFilesPage
}

function nextButton(): HTMLButtonElement {
  const buttons = document.querySelectorAll<HTMLButtonElement>('.pager button')
  expect(buttons.length).toBe(2)
  return buttons[1]!
}

beforeEach(() => {
  vi.clearAllMocks()
})
afterEach(cleanup)

describe('ResultsView keyset pager (FAZ 1.2)', () => {
  it('sends the last row of the current page as the next page cursor', async () => {
    const getFilesPage = mockApi()
    render(<ResultsView filesFound={[]} driveIndex={null} scanId={5} />)
    await waitFor(() => expect(getFilesPage).toHaveBeenCalledTimes(1))
    await waitFor(() => expect(nextButton().disabled).toBe(false))

    fireEvent.click(nextButton())
    await waitFor(() => expect(getFilesPage).toHaveBeenCalledTimes(2))

    // Offset still travels (old-native compatibility), cursor rides the filter.
    expect(getFilesPage.mock.calls[1]![1]).toBe(500)
    const filter = getFilesPage.mock.calls[1]![3] as { cursor: { v: number; id: number } | null }
    // Default sort confidence_desc; last row of page A is id 102 / confidence 70.
    expect(filter.cursor).toEqual({ v: 70, id: 102 })
  })

  it('clears the cursor on sort change and rebuilds it from the new sort', async () => {
    const getFilesPage = mockApi()
    render(<ResultsView filesFound={[]} driveIndex={null} scanId={5} />)
    await waitFor(() => expect(getFilesPage).toHaveBeenCalledTimes(1))
    await waitFor(() => expect(nextButton().disabled).toBe(false))

    // Only the active sort header (confidence, default desc) is non-'none'.
    const sortedTh = document.querySelector('th[aria-sort="descending"]')
    expect(sortedTh).not.toBeNull()
    fireEvent.click(sortedTh!)
    await waitFor(() => expect(getFilesPage).toHaveBeenCalledTimes(2))
    // Fresh page 0 after the sort flip: no cursor, offset back to 0.
    expect(getFilesPage.mock.calls[1]![1]).toBe(0)
    expect((getFilesPage.mock.calls[1]![3] as { cursor: unknown }).cursor).toBeNull()
    expect((getFilesPage.mock.calls[1]![3] as { orderBy?: string }).orderBy).toBe('confidence_asc')

    await waitFor(() => expect(nextButton().disabled).toBe(false))
    fireEvent.click(nextButton())
    await waitFor(() => expect(getFilesPage).toHaveBeenCalledTimes(3))
    const filter = getFilesPage.mock.calls[2]![3] as { cursor: { v: number; id: number } | null }
    // confidence_asc over the same rows: last row of page A is id 101 / conf 90.
    expect(filter.cursor).toEqual({ v: 90, id: 101 })
  })
})

// FAZ 1.3c: the CSV button must delegate to the native single-pass exportCsv
// IPC (dialog-arbitrated path, no renderer file writes) and must NOT keep the
// old 100-round getFilesPage offset walk.
describe('ResultsView CSV export (FAZ 1.3c)', () => {
  const CSV_HEADER_TR = ['Ad', 'Boyut (bayt)', 'Kategori', 'Güven', 'Durum', 'Yol', 'Kaynak', 'Başlangıç Sektörü', 'Oluşturma', 'Değiştirme']

  function mockApiWith(exportCsv: ReturnType<typeof vi.fn>) {
    const getFilesPage = vi.fn(async () => PAGE_A)
    ;(window as unknown as { api: unknown }).api = {
      getFilesPage,
      getFileCount: vi.fn(async () => 2),
      searchFiles: vi.fn(async () => ({ rows: [] })),
      exportCsv,
    }
    return getFilesPage
  }

  function exportButton(): HTMLButtonElement {
    const btn = [...document.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.includes('Dışa Aktar (CSV)'))
    expect(btn).toBeDefined()
    return btn!
  }

  it('calls native exportCsv once and never walks getFilesPage offsets', async () => {
    // Rest args keep the mock's call tuples indexable for the parity asserts.
    const exportCsv = vi.fn(async (..._args: unknown[]) => ({ success: true, rows: 2, path: 'C:/tmp/out.csv' }))
    const getFilesPage = mockApiWith(exportCsv)
    render(<ResultsView filesFound={[]} driveIndex={null} scanId={5} />)
    await waitFor(() => expect(document.querySelector('[data-testid="result-row"]')).not.toBeNull())
    const callsBeforeExport = getFilesPage.mock.calls.length

    fireEvent.click(exportButton())
    await waitFor(() => expect(exportCsv).toHaveBeenCalledTimes(1))
    expect(getFilesPage.mock.calls.length).toBe(callsBeforeExport)

    const [scanId, filter, header, labels, suggestedName] = exportCsv.mock.calls[0]!
    expect(scanId).toBe(5)
    expect((filter as { orderBy?: string }).orderBy).toBe('confidence_desc')
    // Column set/order parity with the native writer (native/tests csvHeader()).
    expect(header).toEqual(CSV_HEADER_TR)
    expect(labels).toEqual({ noFsDate: 'FS tarihi yok', noDate: '—' })
    expect(String(suggestedName).endsWith('.csv')).toBe(true)

    // Row count surfaces when done (role=status report panel).
    await waitFor(() => {
      const report = document.querySelector('[role="status"]')
      expect(report?.textContent).toContain('2')
    })
  })

  it('surfaces a native failure in the exportError alert instead of a report', async () => {
    const exportCsv = vi.fn(async () => ({ success: false, error: 'SQLite error: no such table' }))
    mockApiWith(exportCsv)
    render(<ResultsView filesFound={[]} driveIndex={null} scanId={5} />)
    await waitFor(() => expect(document.querySelector('[data-testid="result-row"]')).not.toBeNull())

    fireEvent.click(exportButton())
    await waitFor(() => expect(exportCsv).toHaveBeenCalledTimes(1))
    await waitFor(() => {
      const alert = document.querySelector('[role="alert"]')
      expect(alert?.textContent).toContain('CSV dışa aktarım başarısız')
    })
    expect(document.querySelector('[role="status"]')).toBeNull()
  })
})
