import { test, expect } from '@playwright/test'
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { launchApp, closeApp } from './helpers'
import { buildFat16TestTxtImage } from './image-fixtures'

const RAID = -1

test('RAID1 from two evidence images recovers FAT16 TEST.TXT', async () => {
  const launched = await launchApp()
  const { win, userData } = launched
  try {
    const img = buildFat16TestTxtImage()
    const a = path.join(userData, 'raid0.img')
    const b = path.join(userData, 'raid1.img')
    writeFileSync(a, img)
    writeFileSync(b, img)
    const destDir = path.join(userData, 'raid-out')
    mkdirSync(destDir, { recursive: true })

    const detected = await win.evaluate(async ({ paths }) => {
      return window.api.detectRaidImages(paths)
    }, { paths: [a, b] })
    expect(detected.found, detected.error).toBe(true)
    expect(detected.raidLevel).toBe(1)

    const assembled = await win.evaluate(async ({ paths }) => {
      return window.api.reconstructRaidImages(paths, 1, 65536, 0)
    }, { paths: [a, b] })
    expect(assembled.success, assembled.error).toBe(true)
    expect(assembled.numDisks).toBe(2)

    const scan = await win.evaluate(async (drive) => {
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
      const id = await window.api.startScan(drive, 'quick')
      if (!(id > 0)) throw new Error(`startScan ${id}`)
      const fin = await done
      return { startedId: id, ...fin }
    }, RAID)
    expect(scan.status).toBe(1)
    const scanId = scan.scanId > 0 ? scan.scanId : scan.startedId

    const files = await win.evaluate(async (sid) => window.api.getFilesPage(sid, 0, 100), scanId)
    const txt = files.find((f) => (f.name ?? '').toUpperCase().includes('TEST'))
    expect(txt, JSON.stringify(files.map((f) => f.name))).toBeTruthy()

    const rec = await win.evaluate(
      async ({ drive, fileId, dest, sid }) => window.api.recoverFile(drive, fileId, dest, sid, false),
      { drive: RAID, fileId: txt!.id, dest: destDir, sid: scanId },
    )
    expect(rec.success, rec.error).toBe(true)
    expect(readFileSync(rec.destPath!).toString('utf8')).toBe('Hello FAT16')
  } finally {
    await closeApp(launched)
  }
})
