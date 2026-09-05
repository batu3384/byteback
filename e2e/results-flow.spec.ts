import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

const FIXTURE_FILES = [
  { name: 'vacation.jpg', path: '/recovered_raw/vacation.jpg', sizeBytes: 4_000_000, confidence: 95, status: 0, source: 'carver', category: 'Image', startSector: 100, endSector: 9000 },
  { name: 'family.png', path: '/recovered_raw/family.png', sizeBytes: 2_000_000, confidence: 88, status: 0, source: 'carver', category: 'Image', startSector: 10000, endSector: 14000 },
  { name: 'shaky.jpg', path: '/recovered_raw/shaky.jpg', sizeBytes: 500_000, confidence: 35, status: 0, source: 'carver', category: 'Image', startSector: 20000, endSector: 21000 },
  { name: 'report.docx', path: 'Users/bat/Documents/report.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 30000, endSector: 30100 },
  { name: 'clip.mp4', path: '/recovered_raw/clip.mp4', sizeBytes: 900_000_000, confidence: 90, status: 0, source: 'carver', category: 'Video', startSector: 40000, endSector: 1800000 },
]

test('seeded scan flows into results triage and unlocks the report nav', async () => {
  const launched = await launchApp()
  const { app, win } = launched

  const scanId = await win.evaluate((files) => window.api.seedScanFixture(files), FIXTURE_FILES)
  expect(scanId).toBeGreaterThan(0)

  // Fresh hydration: the app picks up the latest usable scan on startup.
  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  // Completed scan unlocks the forensic report nav.
  await expect(win.getByTestId('nav-report')).toBeEnabled()
  await expect(win.getByTestId('nav-results')).toBeEnabled()

  await win.getByTestId('nav-results').click()

  // Default filter is "deleted" (metadata only): the single MFT record.
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByTestId('filter-all').click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Confidence column renders tiered chips.
  await expect(win.getByTestId('result-row').first()).toContainText('vacation.jpg')
  await expect(win.getByTestId('confidence-95')).toBeVisible()
  await expect(win.getByTestId('confidence-35')).toBeVisible()

  // Status filter narrows to carved records only.
  await win.getByTestId('filter-carved').click()
  await expect(win.getByTestId('result-row')).toHaveCount(4)
  await win.getByTestId('filter-deleted').click()
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByTestId('filter-all').click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Name search filters live.
  await win.getByLabel('Dosya adı ara').fill('vacation')
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByLabel('Dosya adı ara').fill('')

  // Size/date filter inputs (P0-4) narrow the page live. Fixture sizes:
  // 4MB, 2MB, 0.5MB, 0.12MB and 900MB — min 5MB leaves only the video.
  await win.getByLabel('En küçük boyut MB').fill('5')
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByRole('button', { name: 'Süzgeçleri temizle' }).click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Preserve-paths checkbox (P0-1) toggles next to the recover button.
  await expect(win.getByTestId('preserve-paths')).toBeChecked()

  // Selection counter follows the checkbox.
  await win.locator('tbody input[type="checkbox"]').first().check()
  await expect(win.getByRole('button', { name: /Seçilenleri Kurtar \(1\)/ })).toBeVisible()

  await closeApp(launched)
})

test('language toggle switches the sidebar labels and back', async () => {
  const launched = await launchApp()
  const { win } = launched
  await expect(win.getByTestId('nav-results')).toContainText('Sonuçlar')
  await win.getByRole('button', { name: 'English' }).click()
  await expect(win.getByTestId('nav-results')).toContainText('Results')
  // Restore Turkish so the persisted choice does not leak into other tests.
  await win.getByRole('button', { name: 'Türkçe' }).click()
  await expect(win.getByTestId('nav-results')).toContainText('Sonuçlar')
  await closeApp(launched)
})

test('report page shows the audit chain verification status', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'evidence.docx', path: 'Users/bat/Documents/evidence.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 100, endSector: 300 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  // Completed scan unlocks the report page.
  await expect(win.getByTestId('nav-report')).toBeEnabled()
  await win.getByTestId('nav-report').click()

  // The custody/verify card must resolve to a real chain verdict, not stay
  // stuck on "Doğrulanıyor…".
  const status = win.getByTestId('report-chain-status')
  await expect(status).toBeVisible()
  await expect(status).toContainText(/Doğrulandı|BOZULDU/)

  await closeApp(launched)
})
