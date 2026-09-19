import { test, expect } from '@playwright/test'
import { writeFileSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { launchApp, closeApp } from './helpers'
import { buildFat16TestTxtImage } from './image-fixtures'

const SCAN_IMAGE = -2

test('images FAT16 evidence file to a raw clone via IPC', async () => {
  const launched = await launchApp()
  const { win, userData } = launched
  try {
    const imgPath = path.join(userData, 'fat16.img')
    const destPath = path.join(userData, 'clone.dd')
    const src = buildFat16TestTxtImage()
    writeFileSync(imgPath, src)

    const seeded = await win.evaluate((p) => window.api.seedImageDest(p), destPath)
    expect(seeded).toBe(true)

    const progress = await win.evaluate(async ({ drive, srcPath, dest }) => {
      return new Promise<{ current: number; total: number; error?: string; status?: string }>((resolve, reject) => {
        const off = window.api.onImagingProgress((data) => {
          if (data.error || data.status === 'cancelled' || data.total === 0 ||
              (data.total > 0 && data.current >= data.total)) {
            off()
            resolve(data)
          }
        })
        window.api.startImaging(drive, dest, 'raw', srcPath)
        window.setTimeout(() => {
          off()
          reject(new Error('imaging timeout'))
        }, 30_000)
      })
    }, { drive: SCAN_IMAGE, srcPath: imgPath, dest: destPath })

    expect(progress.error, JSON.stringify(progress)).toBeFalsy()
    expect(progress.total).toBeGreaterThan(0)
    expect(progress.current).toBeGreaterThanOrEqual(progress.total)
    expect(readFileSync(destPath).equals(src)).toBe(true)
  } finally {
    await closeApp(launched)
  }
})
