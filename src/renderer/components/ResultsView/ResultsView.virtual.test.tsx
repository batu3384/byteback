// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render, waitFor } from '@testing-library/react'
import ResultsView from './ResultsView'
import { DEFAULT_ROW_HEIGHT, ROW_OVERSCAN } from './virtual-rows'
import type { FileRecord } from '../../../shared/ipc-contract'

// FAZ 1.3a: the flat table must render only the scroll window (+overscan) of
// the 500-row page while keeping the data-testid="result-row" contract, and
// scrolling must move the window.

function mockApi() {
  const rows: FileRecord[] = Array.from({ length: 500 }, (_, i) => ({
    id: i + 1,
    name: `row-${String(i).padStart(4, '0')}.bin`,
    path: `/x/row-${i}.bin`,
    status: 0,
    sizeBytes: 128,
    confidence: 50,
    source: 'mft',
    createdAt: 100,
    modifiedAt: 110,
  }))
  const getFilesPage = vi.fn(async () => rows)
  ;(window as unknown as { api: unknown }).api = {
    getFilesPage,
    getFileCount: vi.fn(async () => 500),
    searchFiles: vi.fn(async () => ({ rows: [] })),
  }
  return getFilesPage
}

function resultRows(): HTMLElement[] {
  return Array.from(document.querySelectorAll<HTMLElement>('[data-testid="result-row"]'))
}

beforeEach(() => {
  vi.clearAllMocks()
})
afterEach(cleanup)

describe('ResultsView table virtualization (FAZ 1.3a)', () => {
  it('renders only the virtual window and scrolls it', async () => {
    mockApi()
    render(<ResultsView filesFound={[]} driveIndex={null} scanId={7} />)
    await waitFor(() => expect(resultRows().length).toBeGreaterThan(0))

    const el = document.querySelector<HTMLElement>('[data-testid="results-scroll"]')
    expect(el).not.toBeNull()
    // jsdom has no layout: pin the viewport geometry the component reads.
    Object.defineProperty(el!, 'clientHeight', { value: 400, configurable: true })
    Object.defineProperty(el!, 'scrollTop', { value: 0, configurable: true })
    fireEvent.scroll(el!)

    await waitFor(() => {
      const count = resultRows().length
      expect(count).toBeGreaterThan(0)
      // Window = ceil(400/40) visible + 2×overscan; a small constant covers
      // render/frame skew. The full 500-row page must NOT be in the DOM.
      expect(count).toBeLessThanOrEqual(400 / DEFAULT_ROW_HEIGHT + 2 * ROW_OVERSCAN + 4)
      expect(count).toBeLessThan(500)
    })
    expect(resultRows()[0]!.textContent).toContain('row-0000.bin')

    // Scroll to ~row 250: the window must move (first row name changes).
    Object.defineProperty(el!, 'scrollTop', { value: 40 * 250, configurable: true })
    fireEvent.scroll(el!)
    await waitFor(() => {
      const first = resultRows()[0]
      expect(first).toBeDefined()
      expect(first!.textContent).toContain('row-0247.bin')
    })
    // The window is still bounded after the scroll.
    expect(resultRows().length).toBeLessThanOrEqual(400 / DEFAULT_ROW_HEIGHT + 2 * ROW_OVERSCAN + 4)
  })
})
