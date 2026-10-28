import { describe, it, expect, vi, beforeEach } from 'vitest'

const { handle, on, searchFiles } = vi.hoisted(() => ({
  handle: vi.fn(),
  on: vi.fn(),
  searchFiles: vi.fn(),
}))

vi.mock('electron', () => ({
  ipcMain: { handle, on },
}))

vi.mock('./native-bridge', () => ({
  getEngine: () => ({
    getFileCount: vi.fn(),
    getFilesPage: vi.fn(),
    hashEmptyContent: vi.fn(),
    getLatestScanId: vi.fn(),
    getLatestUsableScanId: vi.fn(),
    resetScanDatabase: vi.fn(),
    getScanState: vi.fn(),
    searchFiles,
    searchFileContent: vi.fn(),
    startContentSearch: vi.fn(),
    stopContentSearch: vi.fn(),
    getScanSummary: vi.fn(),
  }),
}))

vi.mock('./ipc-native', () => ({
  callNative: (_ch: string, fn: () => unknown) => fn(),
}))

vi.mock('./session-log', () => ({
  appendSessionLog: vi.fn(),
}))

import { registerScanDbIpcHandlers } from './ipc-handlers-scan-db'

describe('registerScanDbIpcHandlers', () => {
  beforeEach(() => {
    handle.mockClear()
    on.mockClear()
  })

  it('registers scan DB and search channels', () => {
    registerScanDbIpcHandlers()
    const channels = handle.mock.calls.map((c) => c[0])
    expect(channels).toContain('search-files')
    expect(channels).toContain('search-file-content')
    expect(channels).toContain('start-content-search')
    expect(channels).toContain('get-files-page')
    expect(on).toHaveBeenCalledWith('stop-content-search', expect.any(Function))
  })

  it('search-files rejects ReDoS regex before native', async () => {
    registerScanDbIpcHandlers()
    searchFiles.mockClear()
    const entry = handle.mock.calls.find((c) => c[0] === 'search-files')
    const handler = entry?.[1] as (...args: unknown[]) => Promise<{ rows: unknown[]; error?: string }>
    const res = await handler({}, 1, '(a+)+$', 0, 10, true)
    expect(res.rows).toEqual([])
    expect(res.error).toBe('regex rejected')
    expect(searchFiles).not.toHaveBeenCalled()
  })
})
