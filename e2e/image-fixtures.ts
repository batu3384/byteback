/** FAT16 superfloppy matching native `testfix::buildFat16Volume` (TEST.TXT / Hello FAT16). */

export function buildFat16TestTxtImage(): Buffer {
  const ss = 512
  const fatSectors = 16
  const totalSectors = 4120
  const img = Buffer.alloc(totalSectors * ss)
  img[0] = 0xeb
  img[1] = 0x3c
  img[2] = 0x90
  img.write('MSWIN4.1', 3, 'ascii')
  img.writeUInt16LE(ss, 11)
  img[13] = 1
  img.writeUInt16LE(1, 14)
  img[16] = 2
  img.writeUInt16LE(16, 17)
  img[21] = 0xf8
  img.writeUInt16LE(fatSectors, 22)
  img.writeUInt32LE(totalSectors, 32)
  img[38] = 0x29
  img.write('FAT16   ', 54, 'ascii')
  img[510] = 0x55
  img[511] = 0xaa

  const fatStart = 1
  const rootStart = fatStart + 2 * fatSectors
  const dataStart = rootStart + 1
  img.writeUInt16LE(0xffff, fatStart * ss + 4)
  const de = rootStart * ss
  img.write('TEST    TXT', de, 'ascii')
  img[de + 11] = 0x20
  img.writeUInt16LE(2, de + 26)
  img.writeUInt32LE(11, de + 28)
  img.write('Hello FAT16', dataStart * ss, 'ascii')
  return img
}

/** Truncated JPEG (no EOI) at sector 1 — carve + sidecar repair. */
export function buildTruncatedJpegCarveImage(): Buffer {
  const jpeg = Buffer.from([
    0xff, 0xd8, 0xff, 0xdb, 0x00, 0x03, 0x00, 0xff, 0xda, 0x00, 0x02,
    ...Array(20).fill(0),
  ])
  const img = Buffer.alloc(512 * 4)
  jpeg.copy(img, 512)
  return img
}
