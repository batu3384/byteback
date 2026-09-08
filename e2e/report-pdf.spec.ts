import { test, expect } from '@playwright/test'
import { existsSync, readdirSync, readFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { launchApp, closeApp } from './helpers'

// export-report-pdf used to load the report through a data: URL, which fails
// once the encoded HTML passes Chromium's URL length cap (~2MB). The handler
// now writes a temp HTML file and loads it with loadFile. This drives the real
// handler through the UI, asserting the PDF is written and the finally-block
// temp cleanup leaves no byteback-report-*.html behind.
test('report PDF export writes a real PDF and leaves no temp HTML behind', async () => {
  const launched = await launchApp()
  const { app, win, userData } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'evidence.docx', path: 'Users/bat/Documents/evidence.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 100, endSector: 300 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  await expect(win.getByTestId('nav-report')).toBeEnabled()
  await win.getByTestId('nav-report').click()

  // Snapshot the temp dir first: the leak check must measure only this export.
  const tempDir = tmpdir()
  const listTempHtml = () => readdirSync(tempDir).filter((f) => f.startsWith('byteback-report-') && f.endsWith('.html')).length
  const tempHtmlBefore = listTempHtml()

  const pdfPath = path.join(userData, 'exported-report.pdf')
  // Playwright cannot drive the native Windows save dialog — stub it in the
  // main process so the handler writes to a known path.
  await app.evaluate(({ dialog }, p) => {
    dialog.showSaveDialog = async () => ({ canceled: false, filePath: p })
  }, pdfPath)

  await win.getByRole('button', { name: 'RESMİ RAPOR OLUŞTUR' }).click()
  await expect(win.getByText('Rapor Hazır')).toBeVisible()
  await win.getByRole('button', { name: 'PDF Olarak İndir' }).click()
  await expect(win.getByText(/^PDF kaydedildi: /)).toBeVisible({ timeout: 30_000 })

  expect(existsSync(pdfPath)).toBe(true)
  // Real Chromium print output, not an error text file.
  expect(readFileSync(pdfPath).subarray(0, 5).toString('latin1')).toBe('%PDF-')

  expect(listTempHtml()).toBe(tempHtmlBefore)

  await closeApp(launched)
})
