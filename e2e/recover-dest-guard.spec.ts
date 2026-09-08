import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

// Security assessment Fix 1: recovery destinations are validated in the main
// process (src/main/recover-dest-validator.ts) BEFORE the path crosses into
// native — the native destDirIsSafe blocklist alone is bypassable via \\?\
// prefixes and 8.3 short names. A seeded scan fixture makes recover-file /
// recover-files-batch reachable through real IPC without a real drive; every
// blocked destination must come back as a validation error from main.
const FIXTURE_FILES = [
  {
    name: 'report.docx',
    path: 'Users/bat/Documents/report.docx',
    sizeBytes: 120_000,
    confidence: 90,
    status: 0,
    source: 'ntfs_mft',
    category: 'Document',
    startSector: 30_000,
    endSector: 30_100,
  },
]

test('main-process destination policy rejects system, extended-prefix and UNC targets', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    const scanId = await win.evaluate((files) => window.api.seedScanFixture(files), FIXTURE_FILES)
    expect(scanId).toBeGreaterThan(0)

    // Plain system location.
    const system32 = await win.evaluate(
      (sid) => window.api.recoverFile(0, 1, 'C:\\Windows\\System32', sid, false),
      scanId,
    )
    expect(system32.success).toBe(false)
    expect(system32.error).toContain('reddedildi')

    // \\?\ extended-length prefix must not bypass the check (native blocklist
    // gap reproduced through the real IPC surface).
    const extended = await win.evaluate(
      (sid) => window.api.recoverFile(0, 1, '\\\\?\\C:\\Windows\\System32', sid, false),
      scanId,
    )
    expect(extended.success).toBe(false)
    expect(extended.error).toContain('reddedildi')

    // Batch recover: UNC shares are rejected outright.
    const batchUnc = await win.evaluate(
      (sid) => window.api.recoverFilesBatch(0, [1], '\\\\evil\\share\\drop', sid, false),
      scanId,
    )
    expect(batchUnc.succeeded).toBe(0)
    expect(batchUnc.error).toContain('reddedildi')

    // Rejections are recorded in the session log (failed recovery attempts).
    const log = await win.evaluate((sid) => window.api.getSessionLog(80), scanId)
    const rejectLines = log.lines.filter((l) => l.includes('RECOVER_FAIL') && l.includes('dest_rejected'))
    expect(rejectLines.length).toBeGreaterThanOrEqual(3)
  } finally {
    await closeApp(launched)
  }
})
