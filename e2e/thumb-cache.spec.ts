import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

// FAZ 1.3b: gallery thumbnails ride the main-process disk cache through the
// thumb:// protocol. e2e has no imaged drive, so "first visit populates" is
// driven through the same put-thumb IPC the renderer posts after a successful
// preview read; the second visit must render <img src="thumb://..."> through
// the protocol handler AND the CSP img-src thumb: allowance.

const PNG_1PX_B64 =
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='

const IMAGE_FILES = [
  { name: 'cache_a.png', path: '/recovered_raw/cache_a.png', sizeBytes: 100_000, confidence: 90, status: 0, source: 'carver', category: 'Image', startSector: 100, endSector: 300 },
  { name: 'cache_b.jpg', path: '/recovered_raw/cache_b.jpg', sizeBytes: 100_000, confidence: 88, status: 0, source: 'carver', category: 'Image', startSector: 400, endSector: 600 },
]

async function openGallery(win: import('@playwright/test').Page): Promise<void> {
  await win.getByTestId('nav-results').click()
  await win.getByTestId('filter-all').click()
  await win.getByRole('button', { name: 'Galeri' }).click()
}

test('gallery thumbnails are served from the thumb:// disk cache on the second visit', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate((files) => window.api.seedScanFixture(files), IMAGE_FILES)
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  // First visit: cold disk cache — no thumb:// images may exist (the renderer
  // would fall back to data-URL previews; with no drive here, none render).
  await openGallery(win)
  await expect(win.getByText('cache_a.png')).toBeVisible()
  expect(await win.locator('img[src^="thumb://"]').count()).toBe(0)

  // Populate the cache exactly like the renderer does after a preview read.
  const urls = await win.evaluate(async ({ scanId, b64 }) => {
    const page = await window.api.getFilesPage(scanId, 0, 100, { status: 0 })
    const out: string[] = []
    for (const f of page) {
      const url = await window.api.putThumb(f.id, scanId, 'image/png', b64)
      if (url) out.push(url)
    }
    return out
  }, { scanId, b64: PNG_1PX_B64 })
  expect(urls.length).toBe(IMAGE_FILES.length)
  // Host must carry the scan- prefix: pure-digit hosts are canonicalized to
  // IPv4 by Chromium for standard schemes (thumb://5 -> thumb://0.0.0.1).
  for (const url of urls) expect(url.startsWith(`thumb://scan-${scanId}/`)).toBe(true)

  // Second visit: the cache answers and thumb:// images actually decode —
  // naturalWidth > 0 proves the protocol handler served bytes under the CSP.
  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await openGallery(win)

  const thumbImg = win.locator('img[src^="thumb://"]').first()
  await expect(thumbImg).toBeVisible()
  await expect
    .poll(async () => thumbImg.evaluate((el) => (el as HTMLImageElement).naturalWidth), { timeout: 15_000 })
    .toBeGreaterThan(0)
  const thumbCount = await win.locator('img[src^="thumb://"]').count()
  expect(thumbCount).toBe(IMAGE_FILES.length)

  await closeApp(launched)
})
