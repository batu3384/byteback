// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { cleanup, fireEvent, render, waitFor, screen } from '@testing-library/react'
import HexEditor from './HexEditor'

function mockApi(overrides: Record<string, unknown> = {}) {
  ;(window as unknown as { api: Record<string, unknown> }).api = {
    getRaidState: vi.fn(async () => ({
      active: false,
      capacity: 0,
      numDisks: 0,
      level: -1,
      memberDriveIndices: [],
    })),
    readHexData: vi.fn(async () => ({
      data: Array.from({ length: 512 }, (_, i) => i % 256),
    })),
    searchHex: vi.fn(async () => ({ hits: [1000], unread: false })),
    ...overrides,
  }
}

beforeEach(() => {
  vi.clearAllMocks()
})
afterEach(cleanup)

describe('HexEditor hex search', () => {
  it('lists a hit and jumping it opens that sector', async () => {
    mockApi()
    render(<HexEditor driveIndex={0} sectorSize={512} />)
    await waitFor(() => expect(screen.getByTestId('hex-search-query')).toBeTruthy())
    fireEvent.change(screen.getByTestId('hex-search-query'), { target: { value: 'DEADBEEF' } })
    fireEvent.click(screen.getByTestId('hex-search-go'))
    const hit = await screen.findByTestId('hex-search-hit-0')
    expect(hit.textContent).toMatch(/1000/)
    fireEvent.click(hit)
    const sector = document.getElementById('hex-sector') as HTMLInputElement
    expect(sector.value).toBe('1')
  })

  it('shows unread when native reports a skipped window', async () => {
    mockApi({ searchHex: vi.fn(async () => ({ hits: [], unread: true })) })
    render(<HexEditor driveIndex={0} />)
    await waitFor(() => expect(screen.getByTestId('hex-search-query')).toBeTruthy())
    fireEvent.change(screen.getByTestId('hex-search-query'), { target: { value: '00' } })
    fireEvent.click(screen.getByTestId('hex-search-go'))
    await waitFor(() => expect(screen.getByTestId('hex-search-unread')).toBeTruthy())
  })

  it('loads an MFT record view and jumps to its sector', async () => {
    mockApi({
      getMftRecord: vi.fn(async () => ({
        ok: true,
        unread: false,
        mftRef: 0,
        signature: 'FILE',
        flags: 1,
        byteOffset: 4096,
        attrs: [{ type: 0x80, name: '', resident: false }],
      })),
    })
    render(<HexEditor driveIndex={0} sectorSize={512} />)
    await waitFor(() => expect(screen.getByTestId('hex-mft-ref')).toBeTruthy())
    fireEvent.change(screen.getByTestId('hex-mft-ref'), { target: { value: '0' } })
    fireEvent.click(screen.getByTestId('hex-mft-go'))
    await waitFor(() => expect(screen.getByTestId('hex-mft-attr-0').textContent).toMatch(/\$DATA/))
    const sector = document.getElementById('hex-sector') as HTMLInputElement
    expect(sector.value).toBe('8')
  })

  it('bookmarks the current sector and jumps back to it', async () => {
    const stored: Array<{ driveIndex: number; volumePath: string; sector: number; label: string }> = []
    mockApi({
      getHexMarks: vi.fn(async () => stored.slice()),
      setHexMarks: vi.fn(async (marks: typeof stored) => {
        stored.length = 0
        stored.push(...marks)
        return { ok: true }
      }),
    })
    render(<HexEditor driveIndex={0} sectorSize={512} />)
    await waitFor(() => expect(screen.getByTestId('hex-mark-add')).toBeTruthy())
    const sector = document.getElementById('hex-sector') as HTMLInputElement
    const marked = sector.value
    fireEvent.click(screen.getByTestId('hex-mark-add'))
    const row = await screen.findByTestId('hex-mark-0')
    expect(row.textContent).toContain(marked)
    fireEvent.change(sector, { target: { value: '99' } })
    fireEvent.blur(sector)
    fireEvent.click(row)
    expect((document.getElementById('hex-sector') as HTMLInputElement).value).toBe(marked)
  })

  it('loads MFT from initialMftRef without typing the box', async () => {
    const getMftRecord = vi.fn(async () => ({
      ok: true,
      unread: false,
      mftRef: 5,
      signature: 'FILE',
      flags: 1,
      byteOffset: 5120,
      attrs: [{ type: 0x30, name: '', resident: true }],
    }))
    mockApi({ getMftRecord })
    render(<HexEditor driveIndex={0} sectorSize={512} initialMftRef={5} />)
    await waitFor(() => expect(getMftRecord).toHaveBeenCalled())
    expect(getMftRecord).toHaveBeenCalledWith(0, 5, undefined)
    await waitFor(() => expect(screen.getByTestId('hex-mft-view')).toBeTruthy())
    expect((screen.getByTestId('hex-mft-ref') as HTMLInputElement).value).toBe('5')
  })
})
