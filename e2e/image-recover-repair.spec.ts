import { test, expect } from '@playwright/test'
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { launchApp, closeApp } from './helpers'
import { buildFat16TestTxtImage, buildTruncatedJpegCarveImage } from './image-fixtures'

const SCAN_IMAGE = -2

async function scanImage(win: Awaited<ReturnType<typeof launchApp>>['win'], imagePath: string, scanType: string) {
  return win.evaluate(async ({ imagePath: img, scanType: kind, drive }) => {
    const done = new Promise<{ scanId: number; status: number }>((resolve, reject) => {
      const off = window.api.onScanComplete((data) => {
        off()
        resolve(data)
      })
      window.setTimeout(() => {
        off()
        reject(new Error('scan timeout'))
      }, 45_000)
    })
    const id = await window.api.startScan(drive, kind, { imagePath: img })
    if (!(id > 0)) throw new Error(`startScan ${id}`)
    const fin = await done
    return { startedId: id, ...fin }
  }, { imagePath, scanType, drive: SCAN_IMAGE })
}

test('image scan recovers FAT16 TEST.TXT bytes and hex-finds the payload', async () => {
  const launched = await launchApp()
  const { win, userData } = launched
  try {
    const imgPath = path.join(userData, 'fat16.img')
    writeFileSync(imgPath, buildFat16TestTxtImage())
    const destDir = path.join(userData, 'recovered')
    mkdirSync(destDir, { recursive: true })

    const scan = await scanImage(win, imgPath, 'quick')
    expect(scan.status).toBe(1)
    const scanId = scan.scanId > 0 ? scan.scanId : scan.startedId
    expect(scanId).toBeGreaterThan(0)

    const needle = Array.from(Buffer.from('Hello FAT16'))
    const hex = await win.evaluate(
      async ({ drive, needle: n, img }) => window.api.searchHex(drive, n, 8, img),
      { drive: SCAN_IMAGE, needle, img: imgPath },
    )
    expect(hex.hits?.length).toBeGreaterThan(0)

    const files = await win.evaluate(
      async (sid) => window.api.getFilesPage(sid, 0, 100),
      scanId,
    )
    const testTxt = files.find((f) => f.name.includes('TEST'))
    expect(testTxt, JSON.stringify(files.map((f) => f.name))).toBeTruthy()
    expect(testTxt!.id).toBeGreaterThan(0)

    const rec = await win.evaluate(
      async ({ drive, fileId, dest, sid }) => window.api.recoverFile(drive, fileId, dest, sid, false),
      { drive: SCAN_IMAGE, fileId: testTxt!.id, dest: destDir, sid: scanId },
    )
    expect(rec.success).toBe(true)
    expect(rec.destPath).toBeTruthy()
    const payload = readFileSync(rec.destPath!, 'utf8')
    expect(payload).toBe('Hello FAT16')
  } finally {
    await closeApp(launched)
  }
})

test('carve of truncated JPEG recovers original and writes EOI sidecar', async () => {
  const launched = await launchApp()
  const { win, userData } = launched
  try {
    const img = buildTruncatedJpegCarveImage()
    const imgPath = path.join(userData, 'trunc.jpg.img')
    writeFileSync(imgPath, img)
    const destDir = path.join(userData, 'repaired-out')
    mkdirSync(destDir, { recursive: true })
    const jpegSize = 11 + 20

    const scanId = await win.evaluate(async ({ imgPath: p, jpegSize: n }) => {
      return window.api.seedScanFixture([{
        name: 'photo.jpg',
        path: '/recovered_raw/photo.jpg',
        sizeBytes: n,
        confidence: 70,
        status: 0,
        source: 'carver',
        category: 'Image',
        startSector: 1,
        endSector: 1,
        runs: [{ startSector: 1, sectorCount: 1 }],
      }], p)
    }, { imgPath, jpegSize })
    expect(scanId).toBeGreaterThan(0)

    const files = await win.evaluate(
      async (sid) => window.api.getFilesPage(sid, 0, 100),
      scanId,
    )
    const jpeg = files.find((f) => f.name === 'photo.jpg')
    expect(jpeg, JSON.stringify(files.map((f) => ({ name: f.name, source: f.source, runs: f.runs })))).toBeTruthy()

    const rec = await win.evaluate(
      async ({ drive, fileId, dest, sid }) => window.api.recoverFile(drive, fileId, dest, sid, false),
      { drive: SCAN_IMAGE, fileId: jpeg!.id, dest: destDir, sid: scanId },
    )
    expect(rec.success, rec.error).toBe(true)
    expect(rec.destPath).toBeTruthy()
    const destBytes = readFileSync(rec.destPath!)
    expect(destBytes.subarray(-2).equals(Buffer.from([0xff, 0xd9]))).toBe(false)
    expect(rec.repairedPath).toBeTruthy()
    const repaired = readFileSync(rec.repairedPath!)
    expect(repaired.subarray(-2).equals(Buffer.from([0xff, 0xd9]))).toBe(true)
  } finally {
    await closeApp(launched)
  }
})
