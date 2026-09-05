import { describe, it, expect, vi } from 'vitest'

vi.mock('electron', () => ({
  powerSaveBlocker: { start: () => 1, stop: () => {}, isStarted: () => false },
}))

import { initSessionLog, appendSessionLog, readSessionLog, sessionLogPath } from './session-log'
import { mkdtempSync, rmSync, existsSync, readFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'

// appendSessionLog used to grow session.log without bound; it must rotate to
// session.log.old once the file passes the size cap.
describe('session log rotation', () => {
  it('rotates session.log to session.log.old past the size cap', () => {
    const dir = mkdtempSync(join(tmpdir(), 'byteback-slog-'))
    initSessionLog(dir)
    appendSessionLog('BIG', 'x'.repeat(5 * 1024 * 1024))
    appendSessionLog('AFTER_ROTATE', 'tail')

    const rotated = `${sessionLogPath()}.old`
    expect(existsSync(rotated)).toBe(true)
    expect(readFileSync(rotated, 'utf8')).toContain('BIG')

    const tail = readSessionLog(50)
    expect(tail.lines.join('\n')).toContain('AFTER_ROTATE')
    expect(tail.lines.join('\n')).not.toContain('BIG')

    rmSync(dir, { recursive: true, force: true })
  })
})
